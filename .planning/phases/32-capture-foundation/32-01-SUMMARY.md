---
phase: 32-capture-foundation
plan: 01
subsystem: analyzer
tags: [capture-analysis, lambda, free-variables, ast-annotation, stb_ds]

# Dependency graph
requires: []
provides:
  - Iron_CaptureEntry struct in ast.h with name, type, is_mutable fields
  - Iron_LambdaExpr.captures[] and .capture_count annotation fields
  - src/analyzer/capture.h public API declaration
  - src/analyzer/capture.c free variable analysis pass
  - Pipeline integration calling iron_capture_analyze() between typecheck and escape analysis
affects:
  - 32-capture-foundation (32-02 onward: HIR lowering, env structs, emit_c)
  - 33-optimizer-guards
  - 34-mutable-captures

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Capture analysis follows the escape.c intra-procedural walker pattern using stb_ds arrays"
    - "Iron_CaptureEntry defined in ast.h (not capture.h) to avoid circular includes"
    - "Locals set built from lambda params + val/var decls before walking idents"
    - "Nested lambdas processed inner-out so outer lambda's body walk skips inner lambda subtrees"

key-files:
  created:
    - src/analyzer/capture.h
    - src/analyzer/capture.c
  modified:
    - src/parser/ast.h
    - src/analyzer/analyzer.c
    - CMakeLists.txt

key-decisions:
  - "Iron_CaptureEntry placed in ast.h rather than capture.h to avoid circular include (capture.h includes ast.h)"
  - "Nested lambdas: walk_node_for_lambdas stops recursion at IRON_NODE_LAMBDA to avoid double-processing inner lambda idents as outer captures; collect_idents also stops at IRON_NODE_LAMBDA boundaries"
  - "capture analysis emits no diagnostics in Phase 32 — annotation only, no early return after the call"
  - "For loop variable (var_name field on Iron_ForStmt) added to locals set so it is not mistakenly captured"

patterns-established:
  - "Capture analysis pass: collect locals, walk idents, deduplicate via stb_ds string hashmap, arena-alloc final array"

requirements-completed: [FOUND-01]

# Metrics
duration: 40min
completed: 2026-04-02
---

# Phase 32 Plan 01: Capture Foundation Summary

**Free variable analysis pass that annotates every Iron_LambdaExpr with its capture set (name/type/is_mutable) using resolved_sym, integrated between typecheck and escape analysis**

## Performance

- **Duration:** ~40 min
- **Started:** 2026-04-02T18:55:00Z
- **Completed:** 2026-04-02T19:35:12Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Iron_CaptureEntry struct and Iron_LambdaExpr capture fields added to ast.h — no circular include issues
- Full free variable analysis in capture.c: locals set, ident walker, stb_ds deduplication, arena-allocated output array
- Pipeline integration in analyzer.c (Step 3b between typecheck and escape analysis)
- All 173+ existing integration tests pass with zero regressions

## Task Commits

Each task was committed atomically:

1. **Task 1: Create capture analysis data structures and header** - `9801fc6` (feat)
2. **Task 2: Implement capture analysis pass and integrate into pipeline** - `a092ed7` (feat)

**Plan metadata:** (docs commit — to be created)

## Files Created/Modified
- `src/parser/ast.h` - Added Iron_CaptureEntry typedef and captures/capture_count fields on Iron_LambdaExpr
- `src/analyzer/capture.h` - Public API: iron_capture_analyze() declaration
- `src/analyzer/capture.c` - Full free variable analysis implementation (~400 lines)
- `src/analyzer/analyzer.c` - Added #include and Step 3b call to iron_capture_analyze()
- `CMakeLists.txt` - Added src/analyzer/capture.c to iron_compiler source list

## Decisions Made
- Iron_CaptureEntry placed in ast.h (not capture.h) to avoid circular includes since capture.h includes ast.h
- Nested lambda handling: the ident walker stops at inner IRON_NODE_LAMBDA boundaries; the lambda walker processes inner lambdas separately in inner-out order
- For loop variable (Iron_ForStmt.var_name) explicitly added to the locals set
- capture analysis emits no diagnostics — annotation-only pass with no early return guard after the call in analyzer.c

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Capture annotation foundation complete: every Iron_LambdaExpr.captures[] is populated after analysis
- Ready for 32-02: HIR lowering can now read le->captures to wire env parameters and MAKE_CLOSURE operands
- Ready for 32-03: emit_c.c can generate env structs and closure wrappers using the capture set

---
*Phase: 32-capture-foundation*
*Completed: 2026-04-02*
