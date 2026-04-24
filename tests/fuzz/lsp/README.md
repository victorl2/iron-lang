# LSP fuzz harnesses — Phase 7 Plan 07-05 (HARD-19)

Four coverage-guided libFuzzer harnesses targeting the four adversarial-input
attack surfaces of the Iron LSP (`ironls`):

| Harness              | Target code under test                                    |
| -------------------- | --------------------------------------------------------- |
| `fuzz_lsp_frame`     | `src/lsp/transport/frame.c` — Content-Length framer       |
| `fuzz_lsp_json`      | `src/lsp/transport/json.c` — yyjson + Iron_Arena binding  |
| `fuzz_lsp_dispatch`  | `src/lsp/server/dispatch.c` — method table lookup         |
| `fuzz_lsp_didChange` | `src/lsp/store/document.c` — document-sync state machine  |

The compiler-side fuzz targets (`fuzz_parser`, `fuzz_typecheck`,
`fuzz_hir_to_lir`) live in `tests/fuzz/` and cover the `ironc` pipeline.
These are the LSP counterparts for the `ironls` pipeline.

## Seed corpus policy

Hand-crafted at v1 (per `07-CONTEXT.md` §D-07 rejected-alternative: no
mining from live editor traffic — would risk leaking user data into a
public repo). Each harness ships 4–5 hand-chosen seeds covering:

- **empty** — zero-byte input (libFuzzer's lower boundary).
- **valid_minimal / valid_object / sequence_open_close** — smallest
  valid-shape input that lets coverage-guided mutation find neighbouring
  coverage quickly.
- **malformed / oversize / oob_range** — edge cases corresponding to
  specific threat model entries (T-02-02 Content-Length cap,
  T-02-09 OOB range rejection, T-02-13 version monotonicity).
- **utf16_surrogate / combining_marks / deep_nesting / malformed_utf8** —
  encoding-layer pathologies that historically break hand-rolled UTF
  math + recursion limits.

Adding a new seed requires CODEOWNERS review (T-07-05-02 mitigation —
corpora are shipped in a public repo, so any file must be auditable by
a human before merge).

## Local reproduction

```bash
# Configure + build (requires clang; gcc is rejected by the clang-only
# gate at CMakeLists.txt:129-133).
cmake -B build-fuzz -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_BUILD_TYPE=Debug \
    -DIRON_ENABLE_FUZZING=ON

cmake --build build-fuzz --target fuzz_lsp_frame fuzz_lsp_json \
                                  fuzz_lsp_dispatch fuzz_lsp_didChange

# Smoke-run (1000 runs each; completes in < 10 seconds on seed corpus).
./build-fuzz/tests/fuzz/lsp/fuzz_lsp_frame     -runs=1000 tests/fuzz/lsp/frame/corpus/
./build-fuzz/tests/fuzz/lsp/fuzz_lsp_json      -runs=1000 tests/fuzz/lsp/json/corpus/
./build-fuzz/tests/fuzz/lsp/fuzz_lsp_dispatch  -runs=1000 tests/fuzz/lsp/dispatch/corpus/
./build-fuzz/tests/fuzz/lsp/fuzz_lsp_didChange -runs=1000 tests/fuzz/lsp/didChange/corpus/

# Long-form run (matches CI budget — 10 minutes per harness).
./build-fuzz/tests/fuzz/lsp/fuzz_lsp_frame \
    -seed=1 \
    -max_total_time=600 \
    -timeout=10 \
    -max_len=8192 \
    -artifact_prefix=crashes/ \
    tests/fuzz/lsp/frame/corpus/
```

Environment knobs used by CI (mirrored from `.github/workflows/fuzz.yml`):

```
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

`detect_leaks=0` is load-bearing — some LSP TUs (stb_ds token arrays,
yyjson internal pools) intentionally leak at process exit because arena
teardown makes explicit free unnecessary; LSan would flag those.

## Crash triage path

When libFuzzer catches a crash, it writes `crash-<sha1>` into the
`-artifact_prefix` directory. Reproduce locally with:

```bash
./build-fuzz/tests/fuzz/lsp/fuzz_lsp_frame crashes/crash-abc123...
```

Then:

1. `scripts/fuzz_crash_to_fixture.sh --crash-dir crashes/ --target lsp_frame \
   --run-id LOCAL --sha "$(git rev-parse HEAD)"` minimizes the input and
   computes a top-3-frame signature for deduplication (Phase 1 script —
   works unchanged for the new LSP targets).
2. File an issue via the `--open-issue` flag, or file manually with the
   minimized-input + backtrace attached.
3. Fix at the source. Do NOT suppress — the fuzz corpus grows over time,
   and a suppressed finding is a latent bug.
4. Add the minimized crash input to the harness's `corpus/` directory so
   the regression is locked in for future runs.

## Adding a new LSP harness

1. Create `tests/fuzz/lsp/<name>/fuzz_target.c` with
   `int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)`.
2. Create `tests/fuzz/lsp/<name>/corpus/` with 4–5 hand-crafted seeds.
3. Extend `tests/fuzz/lsp/CMakeLists.txt` with a fresh
   `add_executable(fuzz_lsp_<name> ...)` entry; call the two shared
   helper functions (`iron_lsp_fuzz_common_includes`,
   `iron_lsp_fuzz_common_flags`); link whatever iron_* static libs or
   individual TUs the target code needs.
4. Add a matrix entry to `.github/workflows/fuzz.yml` under
   `jobs.fuzz.strategy.matrix.include` with `target`, `binary`, `corpus`,
   `dict: ''` (LSP harnesses use no dict), and `build_target` fields.

## Known limitations (v1)

- **No TSAN coverage.** libFuzzer + ThreadSanitizer is not supported by
  upstream clang; the three-way sanitizer mutex at `CMakeLists.txt:65`
  enforces this structurally. Fuzz harnesses run under ASan + UBSan only.
  Concurrency bugs are caught by the Phase 7 Plan 07-03 TSAN CI job,
  not these fuzz harnesses.
- **`fuzz_lsp_dispatch` does not call `ilsp_dispatch_route`.** The full
  dispatcher requires a fully-wired `IronLsp_Server` (writer thread,
  FILE* sink, document store, workspace index, compiler pipeline). The
  harness instead exercises `ilsp_handler_lookup` directly on the method
  string extracted from adversarial JSON input — which covers the
  ReDoS-class risks (T-07-05-03) without requiring real handler
  invocation. See `dispatch/fuzz_target.c` header comment for full
  rationale.
- **`detect_leaks=0`.** LSan would fail every run because several TUs
  deliberately leak stb_ds backing arrays + yyjson pools at process
  exit (see `src/parser/parser.c:15-49` "FIX-03" comment and yyjson's
  internal pool semantics). These are accepted lifetime tradeoffs; the
  real leak surface is covered by Phase 7 Plan 07-02's soak harness
  (linear-regression growth detection over 8 hours).

## CI integration

`.github/workflows/fuzz.yml` runs the full matrix (3 compiler-side + 4
LSP-side = 7 targets) nightly at `06:00 UTC` on ubuntu-latest. Each
matrix cell gets a fresh runner, a 20-minute job timeout, and a
`-max_total_time=600` (10-minute) fuzz budget. Any crash uploads
`crashes/` as a workflow artifact and opens (or updates) a `fuzz-crash`
labelled issue via `scripts/fuzz_crash_to_fixture.sh`. PRs do NOT run
fuzz — this is a regression net, not a merge gate (per Phase 1 FUZZ-05
decision).
