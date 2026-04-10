---
phase: 58-benchmark-stabilization
plan: 01
subsystem: testing
tags: [stdlib, time, monotonic-clock, nanoseconds, benchmark-runner, clock_gettime, CLOCK_MONOTONIC]

# Dependency graph
requires:
  - phase: 57-soa-fusion-composition
    provides: "318-test stable baseline; no blocking codegen issues"
provides:
  - "Time.now_ns() Iron stdlib API returning int64_t monotonic nanoseconds via CLOCK_MONOTONIC"
  - "Iron_time_now_ns C runtime function mirroring Iron_time_now_ms exactly"
  - "tests/integration/time_now_ns.iron regression test enforcing monotonic/nonzero/ns-granularity invariants"
  - "extract_time_ms() runner helper that prefers 'Total time: X ns' lines and normalizes to ms with 6-decimal precision"
  - "Backwards-compatible ms fallback for C reference outputs (Total time: X.yyy ms)"
affects:
  - 58-02-benchmark-rewrite  # will emit 'Total time: {elapsed_ns} ns' from all 88 Iron benchmarks
  - 58-03-per-problem-audit  # relies on sub-ms precision unlocked by ns timing
  - 58-04-baseline-refresh    # relies on stabilized per-problem thresholds from the audit

# Tech tracking
tech-stack:
  added: []  # No new libraries — pure stdlib extension using existing clock_gettime
  patterns:
    - "Sibling stdlib function addition: Iron_<module>_<func> alongside existing peers in iron_<module>.{c,h} + src/stdlib/<module>.iron"
    - "HIR static-method mangling pathway confirmed: stdlib Time methods route via lowercased-type-name mangling with zero registry changes"
    - "DCE-proof Iron loop bodies: mix in a runtime Time.now_ns() sample inside the loop to defeat C-O2 constant folding of pure-integer accumulations"
    - "Benchmark runner dual-format extraction: prefer precise ns line, fall back to legacy decimal-ms line, normalize internally with awk printf %.6f"

key-files:
  created:
    - tests/integration/time_now_ns.iron
    - tests/integration/time_now_ns.expected
  modified:
    - src/stdlib/time.iron
    - src/stdlib/iron_time.h
    - src/stdlib/iron_time.c
    - tests/benchmarks/run_benchmarks.sh

key-decisions:
  - "CLOCK_MONOTONIC for ns: identical clock source to Iron_time_now_ms; no #ifdef __APPLE__, no mach_absolute_time fallback — Apple supports it natively since macOS 10.12"
  - "int64_t return type: matches Iron's Int (which IS int64_t) and mirrors the ms sibling exactly; tv_sec*1e9+tv_nsec fits comfortably since CLOCK_MONOTONIC references system boot (not Unix epoch)"
  - "HIR mangling pathway suffices: adding func Time.now_ns() -> Int {} stub to time.iron is enough — no src/hir/hir_to_lir.c or builtin-registry edits needed (smoke-confirmed end-to-end on a standalone .iron file)"
  - "Runner takes Option 2 (extend regex), not Option 1 (Iron formats as %.3f ms): Iron has no float format specifier support, so the ns-line path is the only viable approach for Plan 02's Iron benchmark rewrite"
  - "extract_time_ms normalizes ns→ms at 6-decimal (microsecond) precision, safely over-precise vs C's %.3f output; head -1 ensures ns wins over ms when both lines are present"
  - "Test loop DCE resistance: Iron→C→O2 constant-folds pure-integer accumulations even with a read-back. Mixing Time.now_ns() into the accumulator defeats the fold because the clock samples are runtime-dependent"

patterns-established:
  - "Sibling C stdlib function emission: new Iron_time_now_ns lives next to Iron_time_now_ms in iron_time.c with identical clock_gettime(CLOCK_MONOTONIC, ...) plumbing — future time API additions (now_us, now_epoch_ns) follow this exact shape"
  - "Benchmark runner dual-format helper: when adding a higher-precision timing line, prefer it in extract_time_ms, normalize internally, keep the legacy regex as fallback — do not touch any other runner function"
  - "DCE-proof micro-benchmarks in Iron: integer loops that sum/multiply must include a runtime-sampled operand (clock value, argv, env) to prevent the C backend from folding the loop body to a constant"

requirements-completed: [BENCH-01, BENCH-02]

# Metrics
duration: 16min
completed: 2026-04-10
---

# Phase 58 Plan 01: Time.now_ns Foundation Summary

**Landed Time.now_ns() Iron stdlib API with CLOCK_MONOTONIC int64_t nanoseconds, regression test, and a ns-aware extract_time_ms helper in run_benchmarks.sh — unblocking Plans 02-04's benchmark rewrite and per-problem audit.**

## Performance

- **Duration:** 16 min
- **Started:** 2026-04-10T10:49:57Z
- **Completed:** 2026-04-10T11:06:52Z
- **Tasks:** 3
- **Files modified:** 4 modified + 2 created = 6 total

## Accomplishments

- **Time.now_ns() Iron API** live end-to-end: `import time; Time.now_ns()` compiles, lowers via HIR static-method mangling to `Iron_time_now_ns`, returns int64_t nanoseconds from `clock_gettime(CLOCK_MONOTONIC, ...)`. Zero HIR or builtin-registry changes required — the mangling pathway handled it for free.
- **Regression test `time_now_ns.iron` passing**: enforces (1) monotonic (b >= a), (2) nonzero delta across a 10000-iteration busy loop, (3) ns-order-of-magnitude bounds catching accidental ms/us/s unit regressions. Full integration suite: **319 passed / 0 failed** (+1 from the 318 baseline, zero regressions).
- **`extract_time_ms()` extended**: prefers `Total time: <integer> ns` when present (sub-ms precision), normalizes via `awk printf %.6f` to ms, falls back to the legacy `Total time: <number> ms` regex for C reference outputs. Four smoke tests confirm: `14500000 ns → 14.500000`, `15.335 ms → 15.335`, both-lines-present picks ns, `16.262 ms → 16.262` (C-compat).
- **Clean build**: `cmake --build build` exits 0 with no new warnings. `build/ironc` and `build/iron` both present.

## Task Commits

Each task was committed atomically:

1. **Task 1: Add Time.now_ns() to Iron stdlib** — `1b09cb5` (feat)
2. **Task 2: Add time_now_ns regression test** — `34539f6` (test)
3. **Task 3: Extend run_benchmarks.sh extract_time_ms** — `4842833` (feat)

**Plan metadata:** _(final docs commit appended after this summary)_

## Files Created/Modified

### Created
- `tests/integration/time_now_ns.iron` — regression test enforcing monotonic + nonzero + ns-granularity invariants, with DCE-proof loop bodies that mix in runtime Time.now_ns() samples
- `tests/integration/time_now_ns.expected` — 3-line expected output: `monotonic ok / nonzero ok / ns_granularity ok`

### Modified
- `src/stdlib/time.iron` — added `func Time.now_ns() -> Int {}` stub after line 5 (between `now_ms` and `sleep`)
- `src/stdlib/iron_time.h` — added `int64_t Iron_time_now_ns(void);` declaration after `Iron_time_now_ms`
- `src/stdlib/iron_time.c` — added `Iron_time_now_ns()` definition after `Iron_time_now_ms()`, mirrors its structure exactly but with `tv_sec * 1000000000 + tv_nsec`
- `tests/benchmarks/run_benchmarks.sh` — replaced `extract_time_ms()` with ns-preferred + ms-fallback variant; diff strictly scoped to that one block

## HIR Lowering Verification

The plan's critical hypothesis was that `Time.now_ns()` would lower via HIR static-method mangling (lowercasing type name portion) into `Iron_time_now_ns` with **zero registry or builtin-table changes**. Confirmed via standalone smoke test (pre-commit for Task 1):

```
-- /tmp/test_now_ns_smoke.iron
import time
func main() {
    val a = Time.now_ns()
    val b = Time.now_ns()
    if b >= a { println("ok") } else { println("fail") }
}
```

Result: `build/iron build` succeeded, binary printed `ok`. No edits to `src/hir/hir_to_lir.c`, no edits to a builtin/method registry, nothing. The mangling-only pathway works for stdlib Time methods exactly as the plan's `<interfaces>` block predicted.

## Shell Smoke-Test Outputs (Task 3)

Four test cases verifying `extract_time_ms()` behavior:

| Input                                               | Expected   | Got        | Result |
| --------------------------------------------------- | ---------- | ---------- | ------ |
| `Total time: 14500000 ns`                           | `14.500000` | `14.500000` | PASS  |
| `Total time: 15.335 ms`                             | `15.335`   | `15.335`   | PASS   |
| `Total time: 14500000 ns\nTotal time: 14 ms` (both) | `14.500000` | `14.500000` | PASS  |
| `Total time: 16.262 ms` (C reference)               | `16.262`   | `16.262`   | PASS   |

ns-wins-over-ms and ms-only-fallback both confirmed. C backwards compatibility preserved.

## Integration Test Count Delta

| Measure         | Before | After |
| --------------- | -----: | ----: |
| Integration pass | 318   | 319   |
| Integration fail | 0     | 0     |
| Integration total | 324   | 325   |

**+1 test, +0 regressions.** The new `time_now_ns` slot is `[RUN ] time_now_ns ... [PASS]` in the tail of the integration report.

## Decisions Made

- **Clock source = CLOCK_MONOTONIC** (not `CLOCK_MONOTONIC_RAW`, not `mach_absolute_time`): mirrors the existing `Iron_time_now_ms` sibling exactly, works on both Linux and modern macOS, no platform fork required.
- **Return type = int64_t**: matches Iron's `Int` type and the ms sibling; avoids an HIR-level cast that a uint64 return would require.
- **Option 2 (runner regex extension) over Option 1 (Iron formats ms with decimal)**: Iron's string interpolation has no float format specifier support, so producing `Total time: 14.500 ms` from an int64_t nanosecond value would require leading-zero-padded decimal fraction assembly. Extending the runner is a ~10-line shell change that avoids a compiler-feature request.
- **6-decimal ns→ms precision** in `extract_time_ms()`: microsecond granularity in ms units, safely over-precise vs C's `%.3f` output, never rounds a sub-ms C value to zero.
- **Loop bodies mix in runtime `Time.now_ns()` samples** in the regression test: the original plan's loop (`for i in range(10000) { acc = acc + i }` with a read-back `acc`) is constant-folded by the C backend at -O2 despite the read-back — `delta` came back as 0 ns. Fix: `acc = acc + Time.now_ns()` makes the sum depend on runtime-sampled values the compiler cannot fold. This is a reusable pattern for future Iron micro-benchmarks.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 — Bug] Test loop constant-folded by C backend despite read-back**
- **Found during:** Task 2 (time_now_ns.iron regression test first run)
- **Issue:** The plan's Assertion 2 busy loop (`var acc = 0; for i in range(10000) { acc = acc + i }` with `acc` read back in the FAIL diagnostic) was dead-code-eliminated by C -O2 even though `acc` was live. The sum `0..9999 = 49995000` is a compile-time constant and the C backend folded the loop to a single store, making `delta_work = 0` and failing Assertion 2. Assertion 3 failed transitively for the same reason (`delta_ns = 0`).
- **Fix:** Rewrote both loop bodies to accumulate runtime-sampled `Time.now_ns()` values instead of loop-induction-variable integers: `acc = acc + Time.now_ns()`. This forces the C backend to keep the loop (each call is a syscall-level side effect from the compiler's POV). Assertion 2 now measures nonzero delta reliably. Assertion 3's upper bound was also widened from `100000000` (100 ms) to `100000000000` (100 s) because each iteration now performs a real syscall rather than integer addition — the original bound was sized for a constant-time inner loop.
- **Files modified:** `tests/integration/time_now_ns.iron`
- **Verification:** Direct binary run prints `monotonic ok / nonzero ok / ns_granularity ok`; integration harness reports `[RUN ] time_now_ns ... [PASS]`; full suite 319/0.
- **Committed in:** `34539f6` (Task 2 commit — fix folded into the initial commit since it was caught before Task 2 was closed)

### Plan Acceptance-Criteria Imprecisions (Informational)

These are plan-spec counting errors, not implementation bugs. Functional behavior matches the plan's intent; grep-based spot-checks in the plan were mis-scoped.

**A. `grep -n "Iron_time_now_ns" src/stdlib/iron_time.c` expected 2 matches, observed 1.** The plan's action text specified the comment banner as `/* ── Monotonic time (nanoseconds) ─... */` (not containing the literal symbol `Iron_time_now_ns`), so only the function-definition line matches. The implementation follows the plan's action text verbatim — the acceptance grep was miscounted against the plan's own instructions.

**B. `grep -c "Total time:" tests/benchmarks/run_benchmarks.sh` expected 2, observed 5.** The function body naturally contains two regex lines AND two comment lines AND one header banner — 5 total. The acceptance check was counting only the regex lines while ignoring the comments the plan itself required ("Phase 58: Iron benchmarks print BOTH..."). Spirit (ns + ms regexes both present and distinct) is satisfied — confirmed by the ns regex and ms fallback regex grep checks below it.

**C. `grep -q 'printf "%.6f"' tests/benchmarks/run_benchmarks.sh` expected to succeed, observed failure.** The awk invocation uses shell-escaped quotes: `awk "BEGIN { printf \"%.6f\", ... }"`. The literal file content is `printf \"%.6f\"` (with backslashes) because the awk program is embedded in a double-quoted shell string. The runtime awk execution sees `printf "%.6f"` — verified by the four smoke tests all returning 6-decimal ms values. The acceptance grep should have searched for `%.6f` (without quotes) or `printf \\"%.6f\\"` (with escaped quotes). Functional behavior is correct.

**D. `grep -c "Time.now_ns()" tests/integration/time_now_ns.iron` expected exactly 6, observed 9.** The plan's expected count of 6 assumed two `Time.now_ns()` calls per assertion (start + end markers). After the Rule 1 fix (loop bodies now call `Time.now_ns()` internally for DCE resistance), the count grows to 9 — still satisfying the "at least 6" spirit. The exact-6 check was tied to the bug the fix resolved.

---

**Total deviations:** 1 auto-fixed (Rule 1, bug) + 4 informational (plan acceptance-criteria imprecisions)
**Impact on plan:** Single Rule 1 fix kept the test functional. Plan's locked decisions (clock source, return type, runner strategy) were honored verbatim. No scope creep, no architectural changes.

## Issues Encountered

- **C-O2 constant folding of pure-integer Iron loops.** The regression test's first draft hit this on first run. Documented as a reusable pattern (see `patterns-established`) for future Iron micro-benchmarks — always mix in a runtime-sampled operand to defeat the fold.
- **No other issues.** Build was clean on first `cmake --build build` after all three stdlib edits. HIR mangling worked first-try with zero registry work. Runner smoke tests all passed on first invocation.

## User Setup Required

None — no external service configuration, no environment variables, no secrets. Pure stdlib + test + runner additions.

## Next Phase Readiness

- **Plan 02 (benchmark rewrite) unblocked.** `Time.now_ns()` is callable, `run_benchmarks.sh` will consume `Total time: X ns` lines, and the ms fallback is preserved for C reference outputs. Plan 02 can mechanically rewrite all 88 Iron benchmarks to print the ns line as the primary timing.
- **Plan 03 (per-problem audit) has a signal source.** Sub-ms timings will no longer quantize to `iron_ms = 0`. The 20+ problems that previously reported zero will produce real ratios after Plan 02 lands the ns print format.
- **Plan 04 (baseline refresh) has stable inputs.** The per-problem thresholds from Plan 03's audit will be written into `config.json` rationale fields, and the baseline can be regenerated from a stabilized 5-run.
- **No blockers.** Integration suite at 319/0. Build clean. Plan 02 can start immediately.

---

## Self-Check: PASSED

Verified commits and files exist on disk:

- `1b09cb5` Task 1 commit — FOUND in `git log --oneline`
- `34539f6` Task 2 commit — FOUND in `git log --oneline`
- `4842833` Task 3 commit — FOUND in `git log --oneline`
- `src/stdlib/time.iron` contains `Time.now_ns` — FOUND
- `src/stdlib/iron_time.h` contains `Iron_time_now_ns` — FOUND
- `src/stdlib/iron_time.c` contains `Iron_time_now_ns` body — FOUND
- `tests/integration/time_now_ns.iron` — FOUND
- `tests/integration/time_now_ns.expected` — FOUND
- `tests/benchmarks/run_benchmarks.sh` contains `Total time: [0-9]+ ns` — FOUND
- Integration suite 319 passed / 0 failed — VERIFIED via `bash tests/run_tests.sh integration`
- Project build clean — VERIFIED via `cmake --build build` exit 0 with no warnings

---
*Phase: 58-benchmark-stabilization*
*Completed: 2026-04-10*
