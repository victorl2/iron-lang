# Iron LSP ThreadSanitizer harness

Phase 7 Plan 07-03 (HARD-17, D-05) delivers a ThreadSanitizer CI job that
runs `ironls` with two concurrent document workers and a 300-event
mixed-request workload. This directory holds every artefact the TSAN job
needs.

## Layout

| Path | Role |
|------|------|
| `driver.py` | Python stdlib-only driver: spawns ironls under TSAN, runs the 2-worker soak, scans stderr for `WARNING: ThreadSanitizer:`. |
| `gen_workload.py` | Deterministic generator (seed=`20260423`) for `workload.jsonl`. Regenerating produces byte-identical output; CI can re-verify with `diff`. |
| `workload.jsonl` | 300 newline-delimited LSP events. Committed; `driver.py --self-test` asserts the exact count. |
| `suppressions.txt` | TSAN runtime suppressions. Empty at v1 land per D-05. Growth requires CODEOWNERS sign-off + inline rationale per T-07-03-01. |
| `fixtures/iron.toml` + `fixtures/doc_a.iron` + `fixtures/doc_b.iron` | Minimal 2-document cross-referencing workspace. `doc_a` imports symbols from `doc_b` so workspace-index lock paths are exercised. |

## Local reproduction

TSAN requires clang (GCC's TSAN is behind clang's; RESEARCH §TSAN
integration verifies this). The build is a separate directory from any
normal / ASan / coverage build — sanitizer flavours are compile-time
mutually exclusive.

```bash
# 1. Configure a TSAN build.
cmake -B build-tsan -G Ninja \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_BUILD_TYPE=Debug \
    -DIRON_ENABLE_SANITIZERS=ON \
    -DIRON_SANITIZER=thread \
    -DIRON_BUILD_LSP=ON

# 2. Build ironls under TSAN (5-15x slower to compile than a normal build).
cmake --build build-tsan -j4 --target ironls

# 3. Run the 2-worker mini-soak. The default timeout is 5 minutes; TSAN's
#    runtime slowdown eats most of that budget.
python3 tests/lsp/tsan/driver.py \
    --ironls build-tsan/src/lsp/ironls \
    --workload tests/lsp/tsan/workload.jsonl \
    --workers 2 \
    --timeout-sec 300
```

Exit codes:

| Code | Meaning |
|------|---------|
| 0 | Clean run: zero `WARNING: ThreadSanitizer:` lines. |
| 1 | At least one TSAN warning detected. Investigate `stderr` output. |
| 2 | Driver error: missing binary, missing fixtures, darwin refusal, etc. |

To validate the driver logic without rebuilding ironls:

```bash
python3 tests/lsp/tsan/driver.py --self-test
```

The self-test is wired as a CTest invariant (`test_tsan_driver_self_test`,
label `phase-m6-invariant`) so every normal CI build catches detector
regressions in < 1 second.

## Suppression policy

`suppressions.txt` is **empty** at v1 land. Every entry added requires:

1. A CODEOWNERS-reviewed PR.
2. An inline comment on the suppression line explaining:
   - which symbol / file / function is being suppressed,
   - why the flagged access is demonstrably not a race (cite code review
     or a proof sketch),
   - why the race cannot be fixed at the source (e.g. third-party
     vendored code like `yyjson` or `stb_ds` that we do not patch).
3. A link to the failing CI run or local reproduction commit.

If this file grows beyond 5 entries, treat it as a red flag — real races
are likely hiding behind "false positive" claims. Revisit every entry
rather than add the 6th.

## Triage path when TSAN fires

When CI reports a finding:

1. **Reproduce locally** with the exact commands above.
2. **Isolate** the race: narrow `workload.jsonl` to the minimal slice
   that still fires the warning, add the reduced case as a deterministic
   Unity test if possible.
3. **Fix the race at the source**, not via suppression. Most Phase 2
   surfaces (writer queue, mailbox, cancel flag, workspace index) have
   existing `pthread_*` or `_Atomic` patterns to mirror — use those.
4. **Only suppress if the race is in vendored code** we do not own and
   can demonstrate is benign. Document the rationale inline per the
   policy above.

## Known limitations and deferred work

- **macOS TSAN is deferred to v2** (per `07-CONTEXT.md` §D-05 and the
  deferred-ideas note). Apple clang's TSAN is historically flakier than
  upstream clang's — it hits false positives in Libc and Grand Central
  Dispatch internals. `driver.py` refuses to run on `darwin` to avoid
  wasting CI time on noise. `ubuntu-latest` is the v1 platform.
- **GCC TSAN is not used**. The existing `IRON_ENABLE_FUZZING` pattern
  already requires clang; the TSAN job inherits that requirement.
- **libFuzzer + TSAN is unsupported**: both rely on `-fsanitize=*`
  with incompatible shadow-memory layouts. The CMake validation layer
  enforces this — `-DIRON_SANITIZER=thread -DIRON_ENABLE_FUZZING=ON`
  fails fast with `FATAL_ERROR`.
- **Phase 2 POSIX timer audit**: per `07-CONTEXT.md` §specifics, POSIX
  timer signal handlers can trigger TSAN false positives. The Phase 2
  writer uses `iron_cond_timedwait_ms` (condition variable, not
  `timer_create`), which sidesteps the issue. If a future plan adds
  `timer_create` usage, audit it here before merge.

## Related files

- `.github/workflows/tsan.yml` — per-PR CI job that runs this harness.
- `CMakeLists.txt` — `IRON_SANITIZER` selector + compile-flag mapping.
- `tests/lsp/invariant/CMakeLists.txt` — `test_tsan_driver_self_test`
  registration.
