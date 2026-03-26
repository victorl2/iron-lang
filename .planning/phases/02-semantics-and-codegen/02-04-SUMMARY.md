---
phase: 02-semantics-and-codegen
plan: "04"
subsystem: analyzer
tags: [escape-analysis, concurrency, parallel-for, heap, free, leak, rc, semantic-analysis]

requires:
  - phase: 02-03
    provides: type checker with resolved_type on all expressions and declared_type on val/var decls

provides:
  - Escape analysis pass (iron_escape_analyze): sets auto_free/escapes on Iron_HeapExpr nodes
  - Concurrency checking pass (iron_concurrency_check): E0208 for parallel-for outer mutations
  - E0207 for escaping heap values without free/leak
  - E0212/E0213/E0214 for invalid free/leak usage
  - rc values exempt from escape analysis

affects:
  - codegen (must emit auto-free calls at block exit for auto_free=true heap nodes)
  - 02-05+ (all four semantic passes now chain: resolve -> typecheck -> escape -> concurrency)

tech-stack:
  added: []
  patterns:
    - EscapeCtx with stb_ds arrays for heap_bindings/freed_names/leaked_names/escaped_names
    - ConcurrencyCtx with local_names stb_ds array for parallel-body local tracking
    - resolve_quiet pattern in tests: run resolve+typecheck+escape into a throwaway diag list
    - ARENA_ALLOC macro for typed arena allocations in tests

key-files:
  created:
    - src/analyzer/escape.h
    - src/analyzer/escape.c
    - src/analyzer/concurrency.h
    - src/analyzer/concurrency.c
    - tests/test_escape.c
    - tests/test_concurrency.c
  modified:
    - CMakeLists.txt

key-decisions:
  - "02-04: Escape analysis uses stb_ds dynamic arrays for heap_bindings/freed/leaked/escaped; intra-procedural, walks function body twice (collect then classify)"
  - "02-04: Assignment RHS escape detection is conservative — any assignment of a heap-bound name counts as escape; avoids false negatives at cost of some false positives"
  - "02-04: Concurrency checker collects local names (val/var decls + loop var) at parallel-for entry; any assign to non-local name emits E0208 without requiring scope chain"
  - "02-04: resolve_quiet helper in tests runs resolve+typecheck into a throwaway diag list so g_diags starts clean for the pass under test; fixes stb_ds array offset bug"

requirements-completed: [SEM-09, SEM-10]

duration: 13min
completed: 2026-03-26
---

# Phase 02 Plan 04: Escape Analysis and Concurrency Checking Summary

**Intra-procedural escape analysis (auto_free, E0207, E0212-E0214) and parallel-for mutation checker (E0208) completing the four-pass semantic pipeline**

## Performance

- **Duration:** ~13 min
- **Started:** 2026-03-26T02:46:41Z
- **Completed:** 2026-03-26T02:59:14Z
- **Tasks:** 2
- **Files modified:** 7 (6 created, 1 modified)

## Accomplishments

- Escape analysis pass sets `auto_free=true` on non-escaping heap nodes and `escapes=true` with E0207 on escaping nodes without free/leak
- free/leak validation: E0212 (free non-heap), E0213 (leak non-heap), E0214 (leak rc)
- rc values are fully exempt from escape analysis
- Concurrency checker detects outer non-mutex variable mutation in parallel-for bodies (E0208)
- 16 total Unity tests (11 escape + 5 concurrency), all passing
- Full 12-test suite remains green

## Task Commits

1. **Task 1: Implement escape analysis pass** - `a944519` (feat)
2. **Task 2: Implement concurrency checking pass** - `18c3af7` (feat)

## Files Created/Modified

- `src/analyzer/escape.h` - iron_escape_analyze function signature
- `src/analyzer/escape.c` - Full escape analysis: HeapBinding collection, escape detection, auto_free flagging, free/leak validation
- `src/analyzer/concurrency.h` - iron_concurrency_check function signature
- `src/analyzer/concurrency.c` - Parallel-for body analysis: local name tracking, outer mutation detection
- `tests/test_escape.c` - 11 Unity tests covering all escape/free/leak scenarios
- `tests/test_concurrency.c` - 5 Unity tests covering parallel/sequential for mutation patterns
- `CMakeLists.txt` - Added escape.c and concurrency.c to iron_compiler, added test targets

## Decisions Made

- Escape analysis is intra-procedural: two-pass (collect then classify) within each function body. Assignment of heap variable name to any target counts as escape — conservative but avoids false negatives.
- Concurrency checker uses a local-names set rather than scope chain: at parallel-for entry, all val/var decls inside the body plus the loop variable are "local". Any assignment to a name not in this set triggers E0208. This approach works without needing to retain scope chain references post-resolve.
- Tests use `resolve_quiet` helper to run resolve+typecheck into a throwaway diag list, keeping `g_diags` clean for the pass under test. This avoids a stb_ds array offset bug where reset `count` didn't compact the underlying array.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed stb_ds diag list offset bug in tests**
- **Found during:** Task 1 (writing escape tests)
- **Issue:** Resetting `g_diags.count = 0` after resolve/typecheck didn't compact the stb_ds `items` array. When escape analysis appended new diagnostics, `has_error` only scanned indices 0..count-1, missing the newly appended errors at higher indices.
- **Fix:** Introduced `resolve_quiet` helper that runs resolve+typecheck into a fresh throwaway `Iron_DiagList`, leaving `g_diags` completely clean for the pass under test.
- **Files modified:** tests/test_escape.c
- **Verification:** All 11 tests pass including test_heap_escaped_via_assign which was the failing case.
- **Committed in:** a944519 (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - Bug in test infrastructure)
**Impact on plan:** Necessary for test correctness. No scope creep.

## Issues Encountered

- arena_alloc requires 3 arguments (`arena_alloc(a, size, align)`) but initial test code used 2-argument form. Fixed by using the `ARENA_ALLOC(arena, T)` macro throughout manual-AST tests.

## Next Phase Readiness

- All four semantic passes (resolve, typecheck, escape, concurrency) implemented and tested
- AST nodes fully annotated: `resolved_sym`, `resolved_type`, `declared_type`, `auto_free`, `escapes`
- Ready for Phase 02-05: code generation (C backend)
- Codegen must emit `free()` calls at block exit for heap nodes with `auto_free=true`

---
*Phase: 02-semantics-and-codegen*
*Completed: 2026-03-26*
