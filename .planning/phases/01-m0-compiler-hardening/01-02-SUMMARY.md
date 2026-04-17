---
phase: 01-m0-compiler-hardening
plan: 02
subsystem: compiler
tags: [analyzer, error-recovery, c17, hard-02, hard-03, hard-04, hard-10]

# Dependency graph
requires:
  - phase: 01-m0-compiler-hardening
    plan: 01
    provides: iron_analyze_buffer public entry, IronAnalysisMode enum, docs/dev/abort-audit.md
provides:
  - iron_analyze_with_mode public entry (HARD-02/03 mode-aware dispatcher)
  - iron_parser_set_mode + Iron_Parser.mode field (HARD-02 cascade-suppression gate)
  - every analyzer switch-over-kind dispatcher handles IRON_NODE_ERROR + IRON_NODE_COUNT (HARD-04)
  - 7 HARD-10 REPLACE-category IRON_NODE_ASSERT_KIND sites converted per audit
  - tests/unit/test_analyzer_errornode.c (3 tests, phase-m0-invariant)
  - tests/unit/test_analyzer_no_short_circuit.c (1 test, phase-m0-invariant)
affects: [01-03, 01-04, 01-05]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Mode-aware analyzer dispatcher (iron_analyze_with_mode) with CLI delegator for backward compat"
    - "Parser-state-carried IronAnalysisMode + setter function to avoid API-widening"
    - "HARD-10 REPLACE marker comments on every converted IRON_NODE_ASSERT_KIND site for grep traceability"
    - "Canonical IRON_NODE_ERROR + IRON_NODE_COUNT arm pattern copied from resolve.c:819-826 to 7 passes"

key-files:
  created:
    - tests/unit/test_analyzer_errornode.c
    - tests/unit/test_analyzer_no_short_circuit.c
  modified:
    - src/analyzer/analyzer.h (iron_analyze_with_mode declaration)
    - src/analyzer/analyzer.c (short-circuits removed, mode threaded, dispatcher rewritten)
    - src/parser/parser.h (Iron_Parser.mode field + iron_parser_set_mode declaration)
    - src/parser/parser.c (mode init + setter + iron_emit_diag cascade gate)
    - src/analyzer/typecheck.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms; 4 HARD-10 REPLACE)
    - src/analyzer/capture.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms)
    - src/analyzer/init_check.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms)
    - src/analyzer/escape.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms; 1 HARD-10 REPLACE)
    - src/analyzer/concurrency.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms)
    - src/analyzer/web_await_check.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms)
    - src/analyzer/web_top_level_loader_check.c (IRON_NODE_ERROR + IRON_NODE_COUNT arms)
    - src/analyzer/resolve.c (2 HARD-10 REPLACE; existing top-level IRON_NODE_ERROR arm preserved)
    - tests/unit/CMakeLists.txt (2 new test registrations, phase-m0-invariant label)

key-decisions:
  - "iron_analyze becomes a one-line delegator calling iron_analyze_with_mode(CLI) — zero CLI API churn, mode plumbed through without widening the public surface or the many iron_analyze call sites"
  - "Mode lives on Iron_Parser (not a new parameter) — iron_parser_create defaults to CLI, iron_parser_set_mode() is the setter; keeps existing parser_create signature stable (preserved by parser.h and 99+ existing callers)"
  - "Cascade-suppression gate is a single-line change in iron_emit_diag: `if (p->in_error_recovery && p->mode != IRON_ANALYSIS_MODE_LSP) return;` — CLI path bit-identical, LSP path emits every diagnostic"
  - "Comptime short-circuit (iron_comptime_apply gated on error_count == 0) PRESERVED — that guard is semantic (comptime on broken AST is unsafe), not a HARD-03 short-circuit"
  - "HARD-10 asserts at typecheck.c:1887 and 3414 already had pre-assert guards that handle IRON_NODE_ERROR; the redundant IRON_NODE_ASSERT_KIND was removed and the pre-existing guard was tagged HARD-10 REPLACE as the authoritative audit entry"

requirements-completed: [HARD-02, HARD-03, HARD-04, HARD-10]

# Metrics
duration: ~30min
completed: 2026-04-17
---

# Phase 01 Plan 02: Analyzer Error-Tolerance End-to-End Summary

**Made the analyzer error-tolerant: every pass runs unconditionally on parse-error ASTs (HARD-03), every switch-over-kind dispatcher handles `IRON_NODE_ERROR` gracefully (HARD-04), every REPLACE-category `IRON_NODE_ASSERT_KIND` from `docs/dev/abort-audit.md` is converted to an early-return-with-diagnostic (HARD-10), and cascade-suppression is gated OFF in LSP mode while preserved in CLI mode (HARD-02). HARD-11 parity on `tests/integration/` holds: 381/381 `iron check` fixtures pass unchanged.**

## Performance

- **Duration:** ~30 min (3 tasks)
- **Started:** 2026-04-17 (worktree spawn, post-Wave-1)
- **Completed:** 2026-04-17 (final Unity test + integration run)
- **Tasks:** 3 / 3 complete
- **Files modified:** 11 modified + 2 created

## Accomplishments

- `iron_analyze_with_mode(program, mode, arena, diags, ...)` declared in `analyzer.h` and implemented in `analyzer.c`; backward-compat `iron_analyze()` becomes a one-line delegator passing `IRON_ANALYSIS_MODE_CLI`.
- 4 `if (diags->error_count > 0) { ... return; }` short-circuits removed from `iron_analyze` body (lines 32, 39, 59, 69 in the pre-edit file). All passes now run to completion regardless of earlier error counts.
- 1 semantic gate preserved: `iron_comptime_apply` still runs only when `diags->error_count == 0` — this is not a HARD-03 short-circuit, it is a correctness requirement documented inline.
- `Iron_Parser` struct gained an `IronAnalysisMode mode` field; `iron_parser_create` initialises it to `IRON_ANALYSIS_MODE_CLI`; `iron_parser_set_mode()` exposed for callers (used by `iron_analyze_buffer`).
- `iron_emit_diag` cascade-suppression gated on `p->mode != IRON_ANALYSIS_MODE_LSP` — CLI keeps the old `in_error_recovery` suppression path verbatim (HARD-11 invariant) and LSP mode sees every diagnostic.
- `iron_analyze_buffer` now calls `iron_parser_set_mode(&parser, mode)` right after `iron_parser_create` and calls `iron_analyze_with_mode((Iron_Program *)ast, mode, ...)` so mode propagates end-to-end.
- Every switch-over-kind dispatcher in the 7 analyzer passes (typecheck, capture, init_check, escape, concurrency, web_await_check, web_top_level_loader_check) gained explicit `case IRON_NODE_ERROR:` and `case IRON_NODE_COUNT:` arms. `resolve.c` retained its pre-existing canonical arms unchanged.
- All 7 HARD-10 REPLACE-category `IRON_NODE_ASSERT_KIND` sites per the audit were converted:
  - `typecheck.c:1435` (callee_sym->decl_node OBJECT_DECL) → graceful skip + `IRON_DIAG_NOTE` diagnostic
  - `typecheck.c:1607` (fn_sym->decl_node FUNC_DECL) → `goto skip_generic_constraints` with diagnostic
  - `typecheck.c:1887` (sym->decl_node OBJECT_DECL) → redundant assert dropped; pre-existing guard promoted as HARD-10 REPLACE
  - `typecheck.c:3414` (iface_sym->decl_node INTERFACE_DECL) → redundant assert dropped; pre-existing guard promoted as HARD-10 REPLACE
  - `resolve.c:651` (esym->decl_node ENUM_DECL in pattern match) → graceful break + note
  - `resolve.c:775` (esym->decl_node ENUM_DECL in ENUM_CONSTRUCT) → graceful break + note
  - `escape.c:293` (ls->expr IDENT) → redundant assert dropped; pre-existing `kind == IRON_NODE_IDENT` guard authoritative
- One HARD-10 KEEP marker added at `typecheck.c:474` documenting the structural invariant (csym is filtered to `IRON_SYM_INTERFACE` above the assert).
- `tests/unit/test_analyzer_errornode.c` — 3 Unity tests (malformed source CLI mode no-abort, malformed source LSP mode no-abort + diag count parity, 50-level paren nesting no-abort). All pass.
- `tests/unit/test_analyzer_no_short_circuit.c` — 1 Unity test (source with simultaneous resolve + typecheck errors; verifies both diagnostics appear, proving typecheck ran after resolve errors). Pass.

## Task Commits

1. **Task 01-02-01** `d3c2ea0` — feat: thread IronAnalysisMode + remove analyzer short-circuits (HARD-02/03)
2. **Task 01-02-02** `0604ee0` — feat: IRON_NODE_ERROR tolerance + HARD-10 REPLACE conversions (HARD-04/10)
3. **Task 01-02-03** `abb9126` — test: HARD-03 + HARD-04 Unity invariants

## Verification

- `grep -c 'if (diags->error_count > 0)' src/analyzer/analyzer.c` → 0 ✅
- `grep -c 'if (diags.error_count > 0) {' src/cli/check.c` → 0 ✅
- `grep -c 'error_count == 0' src/analyzer/analyzer.c` → 1 (comptime guard preserved) ✅
- `grep -c 'iron_analyze_with_mode' src/analyzer/analyzer.h` → 1 ✅
- `grep -c 'IRON_ANALYSIS_MODE_LSP' src/parser/parser.c` → 1 (cascade-gate) ✅
- `grep -c 'iron_parser_set_mode' src/parser/parser.c` → 1 ✅
- Every 7/7 analyzer passes contains `case IRON_NODE_ERROR:` and `case IRON_NODE_COUNT:` ✅
- `grep -rn 'HARD-10 REPLACE' src/analyzer/` → 7 sites (matches audit row totals: typecheck 4 + resolve 2 + escape 1) ✅
- `ctest -R test_analyzer_errornode` → 3/3 pass ✅
- `ctest -R test_analyzer_no_short_circuit` → 1/1 pass ✅
- `ctest -R test_analyze_buffer_basic` → 3/3 pass (regression: Wave 1 test still green) ✅

## HARD-11 Parity Proof

Integration fixture sweep via `iron check` on every `tests/integration/*.iron`:

```
Before plan 02 (at Wave-1 tip 417681e): PASS=381 FAIL=0
After Task 01-02-01:                    PASS=381 FAIL=0
After Task 01-02-02:                    PASS=381 FAIL=0
After Task 01-02-03:                    PASS=381 FAIL=0
```

381/381 (100%) `iron check` parity across every integration fixture, before and after all three Plan 02 tasks. CLI cascade-suppression still active (HARD-11 preserved), analyzer short-circuits removed but the integration fixtures are all well-formed so the removed short-circuits have no visible effect. The two dimensions of Plan 02's HARD-11 contract both hold:
- Malformed input: before plan 02 analyzer may have reached an abort path; after plan 02 it emits additional diagnostics and survives. Integration fixtures do not exercise this path, so output is byte-identical.
- Well-formed input: no change — every pass runs to completion without short-circuits on 0-error input, result is identical.

Note on environment: clang is not installed on this host; `iron build` cannot link native binaries. That limits verification to `iron check` + Unity unit tests. The 381/381 `iron check` parity is the strongest signal we can extract from this environment. CI runs clang per `.github/workflows/ci.yml` and will cover `iron build` fixtures.

## Decisions Made

- **`iron_analyze` as a delegator, not a widened function**: the existing 9-param `iron_analyze` signature is called from ~6 internal sites (check.c via Plan 01, build.c, and a handful of others). Adding `mode` as a 10th parameter would touch every caller. Instead, `iron_analyze_with_mode` is the full-param variant, and `iron_analyze` delegates with `CLI`. Zero call-site churn.
- **Mode on `Iron_Parser`, not a parameter**: `iron_parser_create` is called in many places and adding a 7th parameter would ripple. Storing mode on the struct with a dedicated `iron_parser_set_mode` setter keeps the call sites stable and is grep-friendly for cross-plan reasoning.
- **Gate cascade-suppression on `!= IRON_ANALYSIS_MODE_LSP`**: the existing `in_error_recovery` flag stays; the ONLY change is that CLI mode keeps the suppression effect and LSP mode bypasses it. This means the CLI user continues to see a clean diagnostic list after a parse error (not drowning in cascade noise), while the LSP client, which has its own dedup logic, gets every diagnostic verbatim.
- **Preserve the comptime guard**: `if (diags->error_count == 0) iron_comptime_apply(...)` is a correctness invariant (const-eval on a broken AST is undefined), not a HARD-03 short-circuit. Removing it would cause comptime crashes on malformed input.
- **HARD-10 conversions vary in shape**: typecheck.c:1887 and 3414 already had correct kind guards above the assert — in those cases the assert was redundant post-Plan-01 and the guard was promoted as the HARD-10 entry. typecheck.c:1435 and 1607 needed new guards built on the existing assert pattern (early-return-with-diagnostic for 1435, goto-past-block for 1607 due to the surrounding generic constraint loop). resolve.c:651/775 inserted a kind-check before the break. escape.c:293 had the ident kind pre-check; the assert is now comment-only.

## Deviations from Plan

**None requiring Rule 1/2/3 auto-fix**. All 3 tasks executed as written. One cosmetic simplification:

### Cosmetic: Two HARD-10 REPLACE sites became "remove redundant assert + tag existing guard"

- **Found during:** Task 02 audit-site walk
- **Issue (non-issue):** typecheck.c:1887 and 3414 already had `if (!sym->decl_node || sym->decl_node->kind != IRON_NODE_EXPECTED_KIND) { ... graceful exit ... }` *above* their asserts. The IRON_NODE_ASSERT_KIND was already dead code — the kind check guarantees it never aborts.
- **Fix:** Removed the redundant `IRON_NODE_ASSERT_KIND` call and added a `/* HARD-10 REPLACE (audit row ...): */` comment above the pre-existing guard, promoting it as the authoritative audit entry. Net: fewer lines of code, no behaviour change.
- **Impact:** 7 total HARD-10 REPLACE markers land as expected (matches audit). Structural outcome is identical to a full rewrite.

### Environmental: gcc -Werror flag suppression patch in build.ninja (inherited from Wave 1)

- **Found during:** Task 01 baseline build
- **Issue:** Pre-existing `-Werror` warnings in unmodified files (`src/lir/lir_optimize.c:1852` unused-but-set-variable, etc.) block gcc builds. Not introduced by this plan.
- **Fix:** Patched `build/build.ninja` FLAGS line with `-Wno-format-truncation -Wno-type-limits -Wno-stringop-overflow -Wno-unused-value -Wno-unused-but-set-variable` — ephemeral, never committed. Same mitigation Wave 1 executor used.
- **Impact:** None on committed tree. CI builds with clang per `.github/workflows/ci.yml` and is unaffected.

**Total source-code deviations:** 0. **Environmental adaptations:** 1 (shared with Wave 1).

## Issues Encountered

- **clang not installed on host** — same as Wave 1. `iron build` cannot link native binaries locally so `ctest -L integration` end-to-end is not runnable here. `iron check` integration parity (381/381) is the strongest signal available. CI is the authoritative parity gate.
- **`test_string_intern_race` link failure** — pre-existing missing `libtsan.so.0.0.0` on host; flagged in Wave 1 SUMMARY. Not a plan issue.

## TDD Gate Compliance

Plan is declared `type: execute`; TDD gates do not apply. The 2 Unity tests were added alongside the implementation in their own commit (`abb9126`) rather than as a separate RED commit — consistent with the `execute` convention established in Plan 01.

## Next Phase Readiness

- **Plan 03 (HARD-05 cancellation poll sites)** can start immediately. Parser now carries `mode`; adding `cancel_flag` alongside it is a pure extension of the Iron_Parser struct. Analyzer passes all run through `iron_analyze_with_mode` which is a natural insertion point for cancel polls at pass boundaries. The `IRON_ERR_CANCELLED=240` code was reserved by Plan 01.
- **Plan 04 (HARD-08 recursion-depth guard + HARD-07 pthread_once + HARD-09 parser audit conversions)** can start. Plan 01's audit doc enumerates every parser.c row (114 REPLACE). The parser's mode field landing in this plan does not interfere with adding `recur_depth` next.
- **Plan 05 (HARD-02 LSP FS gating + HARD-11 parity fixture)** can start. `mode` already propagates through `iron_analyze_with_mode`; Plan 05 just wires it into `iron_comptime_apply` for FS gating, and builds the parity fixture harness.

No blockers for downstream plans.

## Self-Check: PASSED

Verified after SUMMARY write:

- `src/analyzer/analyzer.h` contains `iron_analyze_with_mode` (grep-count = 1) ✅
- `src/analyzer/analyzer.c` contains `iron_analyze_with_mode` body + `iron_analyze` delegator (grep = 3) ✅
- `src/parser/parser.h` contains `IronAnalysisMode  mode;` field + `iron_parser_set_mode` declaration ✅
- `src/parser/parser.c` `iron_emit_diag` gates on `p->mode != IRON_ANALYSIS_MODE_LSP` ✅
- 7/7 analyzer passes contain both `case IRON_NODE_ERROR:` and `case IRON_NODE_COUNT:` ✅
- `grep -c 'HARD-10 REPLACE' src/analyzer/typecheck.c` = 4 ✅
- `grep -c 'HARD-10 REPLACE' src/analyzer/resolve.c` = 2 ✅
- `grep -c 'HARD-10 REPLACE' src/analyzer/escape.c` = 1 ✅
- `tests/unit/test_analyzer_errornode.c` exists (3 Unity tests) ✅
- `tests/unit/test_analyzer_no_short_circuit.c` exists (1 Unity test) ✅
- `tests/unit/CMakeLists.txt` contains 2 new `phase-m0-invariant` registrations ✅
- Commits `d3c2ea0`, `0604ee0`, `abb9126` exist in git log ✅
- `ctest -R test_analyzer_errornode` → 3/3 pass ✅
- `ctest -R test_analyzer_no_short_circuit` → 1/1 pass ✅
- `ctest -R test_analyze_buffer_basic` → 3/3 pass (Wave 1 regression preserved) ✅
- `iron check` on `tests/integration/*.iron` → 381/381 pass (HARD-11 parity) ✅

---
*Phase: 01-m0-compiler-hardening*
*Completed: 2026-04-17*
