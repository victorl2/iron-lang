---
phase: 67-correctness-fixes-+-crash-canaries
plan: 01
subsystem: planning
tags: [audit-verification, correctness, wasm-re-audit, fix-01, fix-02, fix-03, fix-04, reg-02]

# Dependency graph
requires:
  - phase: 65-correctness-audit
    provides: CORRECTNESS-AUDIT.md top-20 must-fix table (original audit)
  - phase: 66-structural-protections-linux-release-ci
    provides: PROT-01..04 + REG-01 + REG-04 landings that closed 13 of the top-20 rows
provides:
  - 67-AUDIT-STATUS.md single source of truth mapping every top-20 row to DONE/OPEN + grep evidence
  - Post-Phase-65 Wasm re-audit across all 6 WebAssembly files
  - Plan assignment table mapping every OPEN row + Wasm findings to plans 67-02..67-08
  - One new H-severity Wasm finding (Wasm-W1 at emit_web.c:278)
affects: [67-02, 67-03, 67-04, 67-05, 67-06, 67-07, 67-08]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Audit verification workflow: grep every DONE-row evidence claim against live source before trusting Phase N-1 summary text"
    - "Wasm re-audit: run the 6 Phase 65 audit dimensions against post-merge WebAssembly files so no audit row predates the target"

key-files:
  created:
    - .planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md
  modified: []

key-decisions:
  - "Verify every DONE-row grep claim against head commit (9792e87) rather than trusting Phase 66 summary text — caught drift in escape.c (251 → 291) and typecheck.c (1785 → 1868) line numbers"
  - "Treat case-label kind proofs (cases in a switch over node->kind) as structurally equivalent to IRON_NODE_ASSERT_KIND for Wasm blind-cast audit — no new findings from web_await_check.c / web_top_level_loader_check.c"
  - "Classify emit_web.c:278 FrameState_* malloc as a new H-severity finding (Wasm-W1) assigned to 67-02 — same class as ranks 3/4 but in web emitter, so one plan covers both native and web generated-code OOM sites"
  - "Route 285 non-top-20 M arena rows across 67-04/05/06 by subsystem instead of one mega-plan — parser/analyzer+comptime/hir+lir+runtime split matches Phase 65 audit plan structure"

patterns-established:
  - "Evidence column for DONE rows must cite a commit SHA AND a grep verification (not just the Phase N-1 summary — it drifts)"
  - "Wasm re-audit dimensions match Phase 65 exactly so any future target-addition plan can reuse the 6-dimension checklist"

requirements-completed: []  # This plan produces a verification doc; FIX-01/02/03/04/REG-02 are completed by downstream plans 67-02..67-08

# Metrics
duration: 6min
completed: 2026-04-13
---

# Phase 67 Plan 01: Audit Status Verification Summary

**Single source of truth audit-status doc enumerating 13 DONE + 7 OPEN top-20 rows + 1 new Wasm H-severity finding (emit_web.c:278 unchecked FrameState_* malloc) with full plan assignment table.**

## Performance

- **Duration:** ~6 min
- **Started:** 2026-04-13T15:04:03Z
- **Completed:** 2026-04-13T15:10:11Z
- **Tasks:** 1
- **Files modified:** 1

## Accomplishments

- Verified every top-20 row claim by grepping live source at head commit `9792e87` — 13 rows DONE (ranks 5-13, 16, 17, 18, 20), 7 rows OPEN (ranks 1, 2, 3, 4, 14, 15, 19) — matches the 67-RESEARCH.md expectation but with corrected line numbers (escape.c 251 → 291, typecheck.c 1785 → 1868, resolve.c 674/680 → 726/745, comptime.c 474 → 493 are all post-Phase-66 drift)
- Re-audited all 6 post-merge Wasm files (emit_web.c, web_main_loop_split.c, web_await_check.c, web_top_level_loader_check.c, build_web.c, iron_time_web.c) across the same 6 dimensions as Phase 65; found exactly one new H-severity row (Wasm-W1) — emit_web.c:278 generated-code malloc of FrameState_* without NULL check, same class as ranks 3 + 4 but in the web main-loop wrapper emitter
- Produced plan-assignment table mapping every OPEN row + Wasm-W1 to plans 67-02 through 67-08 so downstream executors never re-fix a closed row or miss an open one
- Verified build_web.c host-side allocation safety: all 16 `malloc`/`strdup` call sites (lines 41, 46, 90, 131, 248, 252, 515, 648, 695, 705, 717, 728, 740, 889, 910, 921) are explicitly NULL-checked with full error-path cleanup — no new findings
- Confirmed web_await_check.c + web_top_level_loader_check.c blind-cast safety: all 24 `(Iron_<Kind>*)node` casts sit inside `case IRON_NODE_<Kind>:` labels within `switch ((int)(node->kind))` — structurally kind-proven even without explicit `IRON_NODE_ASSERT_KIND`

## Task Commits

1. **Task 1: Verify top-20 audit rows and build status table** - `6f81087` (docs)

## Files Created/Modified

- `.planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` - 160-line verification artifact: top-20 status table (20 rows, 13 DONE + 7 OPEN with grep evidence), Wasm re-audit section covering all 6 Wasm files across 6 dimensions (1 new H finding: Wasm-W1), plan assignment table distributing OPEN rows + Wasm-W1 across plans 67-02..67-08, and Phase 67 plan count distribution overview

## Decisions Made

- **Grep every DONE-row evidence against live source before trusting summaries.** Phase 66 summary text referenced pre-Phase-66 line numbers (e.g. `typecheck.c:1360` for rank 5, `escape.c:251` for rank 12) which drifted under post-Phase-66 insertions. Re-verified all 13 DONE rows and documented post-drift line numbers (1416, 1868, 726, 745, 238+273, 2813+3196, 291, 611-618). Protects downstream 67-02..67-08 from grep-miss confusion.
- **Case-label switches count as kind guards.** The Wasm blind-cast scan found 24 `(Iron_<Kind>*)node` casts in web_await_check.c and web_top_level_loader_check.c, all sitting inside `case IRON_NODE_<Kind>:` branches of `switch ((int)(node->kind))`. The case label IS the kind proof — adding `IRON_NODE_ASSERT_KIND` would be redundant. Documented this as audit policy so future Phase 67 M-severity walkthrough plans don't flag them.
- **One new H finding (Wasm-W1) routed to 67-02.** emit_web.c:278 emits `(FrameState_%s *)malloc(sizeof(FrameState_%s))` into generated C without a NULL check before `memset(state, 0, ...)` on line 281. This is generated-code OOM UB of the same class as ranks 3 + 4 (native HEAP_ALLOC / RC_ALLOC). Assigning to 67-02 means one plan ships the `iron_oom_abort` helper + all 5 generated-code OOM sites (native + web).

## Deviations from Plan

None - plan executed exactly as written. The plan's `<read_first>` checklist was followed, every grep listed in the Evidence column was executed, the Wasm re-audit covered every file and every dimension, and the plan-assignment table was produced in the exact format specified.

## Issues Encountered

- `.planning/` is gitignored at project root (via `chore: untrack .planning/ directory (again)` commit `8628207`), yet `.planning/phases/*/` files are individually tracked. New files in that tree need `git add -f` to stage through the ignore rule. Used `git add -f .planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` to commit; this matches the convention observed in prior Phase 66 docs commits.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- 67-02 can read 67-AUDIT-STATUS.md, find the 5 H-severity alloc rows assigned to it (ranks 1, 2, 3, 4 + Wasm-W1), and land the `iron_oom_abort` helper + all 5 fix sites in one plan
- 67-03 has a clean row list (14, 15, 19) for the integer-safety + enum-switch tail work
- 67-04/05/06 FIX-02 arena walkthroughs have subsystem boundaries documented so they don't overlap
- 67-07 has the 16 AUDIT-04 rows pre-assigned for FIX-03
- 67-08 has the 15 REG-02 canary-fixture assignment recorded
- Zero source tree modifications — `git status --short src/ tests/` returns empty

## Self-Check

Verified all claims:

- `test -f .planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` → FOUND
- `git log --oneline | grep 6f81087` → FOUND (docs(67-01): audit status verification doc)
- `grep -c "^| [0-9]* |" 67-AUDIT-STATUS.md` → 27 (≥ 20 required — includes distribution subtotal table)
- `grep -c "OPEN" 67-AUDIT-STATUS.md` → 11 (≥ 7 required)
- `grep -c "DONE" 67-AUDIT-STATUS.md` → 16 (≥ 13 required)
- `grep -c "Wasm Re-Audit" 67-AUDIT-STATUS.md` → 1 (== 1 required)
- `grep -c "Plan Assignment" 67-AUDIT-STATUS.md` → 1 (== 1 required)
- `grep -c "src/lir/emit_web.c" 67-AUDIT-STATUS.md` → 4 (≥ 1 required)
- `grep -c "67-02\|67-03" 67-AUDIT-STATUS.md` → 20 (≥ 7 required)
- `git status --short src/ tests/` → empty (zero source edits)

## Self-Check: PASSED

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Completed: 2026-04-13*
