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
