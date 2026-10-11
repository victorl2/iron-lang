# Fuzzing

Iron's compiler and language server are fuzzed with libFuzzer, nightly in CI
and by hand before larger changes land. This page covers what is fuzzed, how
to run it, how to read and triage a failure, and how to turn a finding into a
fix with a regression test.

## Targets

| Target | Binary | Input | What it drives |
|---|---|---|---|
| `parser` | `build/tests/fuzz/fuzz_parser` | Iron source bytes | lexer and parser |
| `typecheck` | `build/tests/fuzz/fuzz_typecheck` | token blob (`tests/fuzz/iron_gen.c`) | parser, resolve, typecheck |
| `hir_to_lir` | `build/tests/fuzz/fuzz_hir_to_lir` | token blob | the above, HIR lowering, HIR to LIR |
| `check` | `build/tests/fuzz/fuzz_check` | Iron source bytes | analysis as `ironc check` and the language server run it (stdlib prelude, broken buffers included), formatter idempotence |
| `lsp_frame` | `build/tests/fuzz/lsp/fuzz_lsp_frame` | bytes | Content-Length framing |
| `lsp_json` | `build/tests/fuzz/lsp/fuzz_lsp_json` | bytes | JSON parsing into the arena |
| `lsp_dispatch` | `build/tests/fuzz/lsp/fuzz_lsp_dispatch` | bytes | method lookup and routing |
| `lsp_didChange` | `build/tests/fuzz/lsp/fuzz_lsp_didChange` | bytes | document edits |

The compiler targets start from a corpus generated at build time
(`iron_seed_blobs`, written to `build/tests/fuzz/corpus/<target>/`) and use the
dictionary `tests/fuzz/iron.dict`. The LSP corpora are in the tree under
`tests/fuzz/lsp/<target>/corpus/`.

`tests/fuzz/iron_gen.c` turns token kinds into source text. It switches over
every token kind, and it is compiled in every normal build
(`iron_fuzz_gen_check`), so adding a keyword or token without teaching the
generator about it fails the build under `-Werror=switch-enum`. Add the new
spelling to `iron_gen_keyword_lit` and, for keywords, to `iron.dict`.

## Nightly CI

`.github/workflows/fuzz.yml` runs every target for 10 minutes each night
(06:00 UTC) and on demand:

```sh
gh workflow run fuzz.yml --ref <branch>
```

Run it on a branch that touches the lexer, parser, analyzer or lowering before
merging; it does not run on pull requests otherwise.

* **Seed.** The seed is the workflow run number, so each night explores new
  paths. It is printed at the start of the fuzz step (`seed=N`).
* **Corpus.** New coverage is written to `fuzz-corpus/<target>`, which is
  cached across runs, so the corpus keeps growing night over night.
* **Limits.** `-timeout=10` (a single input running longer is a hang),
  `-malloc_limit_mb=2048` (a single allocation larger than that is a bug),
  `-rss_limit_mb=4096` (total memory), `-max_len=8192`.
* **On a failure**, the artifacts are uploaded as `fuzz-crashes-<target>-<run>`,
  `scripts/fuzz_crash_to_fixture.sh` minimizes each one and opens (or comments
  on) a `fuzz-crash` issue keyed by the top three stack frames, and the job
  fails.

## Running locally

Fuzzing needs clang with libFuzzer. Run it on Linux in a container with a
memory cap, for example
`podman run --rm --memory=32g -v "$PWD":/work:Z -w /work iron-dev bash`.

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_COMPILER=clang -DIRON_ENABLE_FUZZING=ON
cmake --build build-fuzz --target fuzz_parser fuzz_typecheck fuzz_hir_to_lir \
      iron_seed_blobs fuzz_lsp_frame fuzz_lsp_json fuzz_lsp_dispatch fuzz_lsp_didChange

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
mkdir -p crashes
build-fuzz/tests/fuzz/fuzz_parser -seed=11 -max_total_time=1200 \
    -timeout=10 -max_len=8192 -rss_limit_mb=4096 -malloc_limit_mb=2048 \
    -dict=tests/fuzz/iron.dict -artifact_prefix=crashes/parser- \
    build-fuzz/tests/fuzz/corpus/parser
```

Run the three compiler targets in parallel, each with two or more different
seeds (`-seed=11`, `-seed=23`, ...): every new seed tried so far reached code
the earlier ones had not. Twenty minutes per seed is a good pre-merge round
for a parser or lowering change.

## Reading a failure

libFuzzer names the artifact after what happened:

| Artifact | Meaning | First step |
|---|---|---|
| `crash-<hash>` | ASan or UBSan report, abort, or ICE | replay it and read the stack |
| `timeout-<hash>` | one input ran past `-timeout` | replay it; usually a loop that does not consume a token |
| `oom-<hash>` with content | one input allocated too much | replay with `-malloc_limit_mb=512` to get the allocating stack |
| `oom-da39a3ee...` (empty input) | memory grew across many inputs | a leak per input, see below |
| `leak-<hash>` | LeakSanitizer report (only with `detect_leaks=1`) | read the allocation stack |

`da39a3ee5e6b4b0d3255bfef95601890afd80709` is the SHA-1 of the empty input:
when an `oom-` artifact has that name, no single input is to blame.

Replay one input, and shrink it:

```sh
build-fuzz/tests/fuzz/fuzz_parser -runs=1 crashes/parser-timeout-...
build-fuzz/tests/fuzz/fuzz_parser -minimize_crash=1 -timeout=3 \
    -max_total_time=300 -exact_artifact_path=min crashes/parser-timeout-...
```

For the `parser` target the input is Iron source, so the minimized file can be
fed to `ironc check` directly. libFuzzer's minimization keeps whatever still
triggers the failure, which may still be noise; reducing it further with a
line-then-character delta debugger (drop a chunk, keep the drop if
`ironc check` still hangs or crashes under a 1 GB `ulimit -v`) usually ends in
a readable reproducer of a few dozen bytes. Mount the tree at the path the
build used when running `ironc` in a container, or it cannot find its stdlib.

## Leaks per input

An empty `oom-` artifact means something leaks on every input. To find it,
run the target with LeakSanitizer on for a short while:

```sh
ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0:malloc_context_size=8 \
    build-fuzz/tests/fuzz/fuzz_hir_to_lir -runs=3000 build-fuzz/tests/fuzz/corpus/hir_to_lir
```

and group the reported stacks by their first frame outside `stb_ds.h`. For the
analysis as the LSP runs it, link a small driver against the Debug libraries
(`-fno-omit-frame-pointer -fno-inline`) that calls `iron_analyze_buffer` on
every `tests/integration/v4/*/*.iron`, frees the diagnostics and the arena,
and run it under the same options; whatever LeakSanitizer reports is leaked on
every edit in the language server.

Heap structures that hang off arena objects (stb_ds arrays and maps) are
released with the arena through `iron_arena_on_free` and `iron_arena_own_arr`
(`src/util/arena.h`). `tests/unit/test_arena_hooks.c` runs with leak detection
on in Linux sanitizer builds; extend it when a new structure is handed to the
arena.

## Common causes

* **A recovery loop that does not consume.** A body loop that reports an error
  and calls `iron_parser_sync_stmt` stops on statement keywords (`val`, `if`,
  `return`, ...) without moving, and the loop reports the same error forever.
  Use `iron_parser_sync_member`, which always moves forward, or guard the loop
  with the position at its start (`if (p->pos == start) break;`). Interface,
  patch and match bodies and function type parameters had this.
* **Appending to an array while iterating it.** stb_ds `arrput` may move the
  array; a loop holding the old pointer reads freed memory. Lowering a nested
  `defer` did this.
* **Unbounded recursion** in lowering (`return` inside `defer` re-entered the
  defer cleanup forever).
* **Harness teardown.** A target that does not free what it builds (token
  arrays, HIR and LIR modules) leaks per input; teardown should mirror
  `src/cli/build.c`.

## The tools: language server, formatter, tree-sitter grammar

Three Python fuzzers drive the tools the way editors and users do. They
need only Python 3 and a build (`ironls`, `ironc`); run them against an
ASan+UBSan build. Each takes `--seed` and is deterministic for a seed
(apart from timing), writes failing cases under `--out`, and has a short
fixed-seed ctest entry (`lsp_fuzz_short`, `fmt_fuzz_short`,
`test_tree_sitter_differential`).

**`tests/fuzz/lsp_fuzz.py`** plays editing sessions against `ironls`:
documents from the corpus and from the `prog_fuzz.py` generators, random
incremental edits (typing one character at a time, pasting fragments of
other files, deleting spans, unbalanced braces, half-typed strings and
interpolations, characters outside the BMP), several changes in one
notification, close/reopen churn, `$/cancelRequest`, and requests of every
kind at random positions (inside surrogate pairs and past the end of lines
too) under the utf-16 and utf-8 encodings. It checks that the server does
not exit, hang (per-request deadline) or print a sanitizer report, that
it does not drift from the client's text (it sends ironls's `contentHash`
extension with every change), that responses are well formed (ranges
inside the document and on character boundaries, no overlapping edits,
semantic tokens that decode, no duplicate diagnostics, parents containing
children), and that the published diagnostics agree with `ironc check`.

```sh
python3 tests/fuzz/lsp_fuzz.py --ironls build/ironls --ironc build/ironc \
    --seed 1000 --sessions 200 --steps 400 -j 6 --out lsp-fuzz
# a failing session, replayed and reported (unanswered requests, crash
# notices, sanitizer reports, peak RSS):
python3 tests/fuzz/lsp_fuzz.py --ironls build/ironls --replay lsp-fuzz/fail/<seed>/session.jsonl
# the server's diagnostics for one file next to ironc check's:
python3 tests/fuzz/lsp_fuzz.py --ironls build/ironls --ironc build/ironc --show file.iron
```

`at_failure_<name>.iron` in a failure directory is the text the failing
response was computed on. On a file that does not parse, `ironc check`
and the server report different follow-on errors by design (the CLI
drops a broken statement, the server keeps the partial tree), so only the
first syntax error is compared there.

**`tests/fuzz/fmt_fuzz.py`** formats corpus and generated programs and
perturbations of them (indentation, tabs, trailing whitespace, blank
lines, comments, CRLF, final newline) and checks idempotence, that every
comment survives, that diagnostics are unchanged, that `ironc fmt` refuses
only files that do not parse, that a perturbation of horizontal whitespace
or line ends formats like the original, and with `--ironls` that
`textDocument/formatting` and a whole-document `rangeFormatting` give what
`ironc fmt` writes. `--run` builds and runs each program before and after
formatting and compares the output.

**`tests/fuzz/ts_diff.py`** gives the tree-sitter grammar and the
compiler's parser the same generated programs and mutants of them and
reports where they disagree: `ts-rejects` (the compiler accepts, the
grammar has ERROR nodes: an editor marks valid code) and `ts-accepts`
(the grammar is looser than the language). `--reduce FILE --keep REGEX`
shrinks a disagreement. It needs `tree-sitter` (0.26.x, as the committed
parser; `tree-sitter generate --js-runtime native` regenerates it without
Node).

## Turning a finding into a fix

1. Reduce the input to a readable reproducer and confirm it fails on the
   current build (`ironc check` hangs, crashes or aborts).
2. Fix the cause, and look for the same pattern elsewhere (the other body
   loops, the other callers).
3. Add a regression fixture:
   * a program that must be rejected goes to
     `tests/integration/v4-fail/regressions/<name>.iron` with the expected
     diagnostic code in `<name>.expected`. If the output contains a syntax
     error (E0002, E0101, E0102), the first one must be the expected code;
     `scripts/grammar_check.py --negative` also requires the manual's grammar
     to reject fixtures whose expected code is a parse error;
   * a program that must run goes to `tests/integration/v4/regressions/` with
     its exact output in `<name>.expected`, and should pass the valgrind leak
     oracle (`tests/oracles/leak_check.sh`).
4. Confirm the fixture fails without the fix and passes with it, replay the
   original artifact, and run the full suite, including the ASan/UBSan build.
5. Fuzz the affected targets again with new seeds before merging; fixes have
   uncovered the next bug behind them more than once.

## Program fuzzers

`tests/fuzz/prog_fuzz.py` generates whole Iron programs, computes their
expected output with a Python model written from the manual, compiles them
with `ironc` and compares (its docstring lists the areas and options). The
statement-model areas (`stmt`, `coll`, `obj`, `closure`, `conc`, `arena`,
`num`, `generic`, `ptr`, `comptime`) share `pf_lang.py`; the areas built on
`pf_seg.py` assemble a program from independent segments whose output is
computed while generating: `sync` (locks, channels and tasks with owning
elements, panics in tasks), `arenax` (arenas with interfaces, generics,
closures and threads), `jsonp`, `crypto` and `http` (the standard library
modules, a server and its clients in one program).

```sh
python3 tests/fuzz/prog_fuzz.py sync build/ironc /tmp/pf 1 500 -j8 --runs 2
python3 tests/fuzz/prog_fuzz.py jsonp build/ironc /tmp/pf 1 200 -j4 --valgrind
python3 tests/fuzz/prog_fuzz.py sync build/ironc /tmp/pf 1 200 -j4 --tsan /tmp/rt_tsan
python3 tests/fuzz/prog_fuzz.py conc build/ironc /tmp/pf 1 50 -j2 --helgrind /tmp/rt_vg
python3 tests/fuzz/prog_fuzz.py http build/ironc /tmp/pf 7 --reduce
```

`--asan RTDIR` and `--tsan RTDIR` build a sanitizer runtime into RTDIR
(OpenSSL included where its headers are installed). `--helgrind RTDIR` and
`--drd RTDIR` build programs and runtime at `-O0` with the runtime's
happens-before annotations (`IRON_VALGRIND_ANNOTATE`) and run with
`tests/fuzz/valgrind_threads.supp`: neither tool models C11 atomics, and
without these every report is a refcount, a string literal's one-time
initialization or glibc's thread stack cache. A failing program is kept
under `OUTDIR/fail/`; `--reduce` shrinks it (segment programs lose whole
segments). The `crypto` area cross-checks AEAD ciphertexts with Python's
`cryptography` package when it is installed (`pip install --target DIR
cryptography` and `PYTHONPATH=DIR` work in the iron-dev container).
