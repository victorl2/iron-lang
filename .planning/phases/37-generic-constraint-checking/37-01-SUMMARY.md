---
phase: 37-generic-constraint-checking
plan: 01
subsystem: analyzer
tags: [generics, constraints, interfaces, type-checking, parser]

# Dependency graph
requires:
  - phase: 33-type-validation-checks
    provides: type checking infrastructure and diagnostic emission patterns
provides:
  - Generic constraint parsing (T: InterfaceName syntax in generic param lists)
  - Constraint validation at function call, call-as-construction, and ConstructExpr sites
  - IRON_ERR_GENERIC_CONSTRAINT (206) diagnostic for constraint violations
affects: [37-02-generic-constraint-checking, 38-concurrency-analysis]

# Tech tracking
tech-stack:
  added: []
  patterns: [nominal-plus-structural constraint satisfaction, generic-param-to-concrete-type inference from call args]

key-files:
  created: []
  modified:
    - src/parser/ast.h
    - src/parser/parser.c
    - src/analyzer/typecheck.c

key-decisions:
  - "constraint_name field added to Iron_Ident (not a separate node) -- minimal AST change"
  - "Constraint satisfaction uses both nominal (implements) and structural (has-all-methods) checks"
  - "Concrete type inference for function calls matches generic param names against formal param type annotations"
  - "Max 16 generic params per declaration (stack-allocated array avoids heap allocation)"

patterns-established:
  - "Generic constraint pattern: parse constraint name on Iron_Ident, check at instantiation site via type_satisfies_constraint + check_generic_constraints helpers"

requirements-completed: [GEN-01, GEN-02, GEN-03, GEN-04]

# Metrics
duration: 9min
completed: 2026-04-03
---

# Phase 37 Plan 01: Generic Constraint Checking Summary

**Parser accepts T: InterfaceName constraint syntax; type checker validates constraints at call/construct sites via nominal and structural interface satisfaction**

## Performance

- **Duration:** 9 min
- **Started:** 2026-04-03T10:34:13Z
- **Completed:** 2026-04-03T10:44:12Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments
- Added constraint_name field to Iron_Ident and parser populates it from T: ConstraintName syntax
- Implemented type_satisfies_constraint with both nominal (implements) and structural (has-all-methods) checks
- Constraint validation at three sites: function calls, call-as-construction, and ConstructExpr
- IRON_ERR_GENERIC_CONSTRAINT (206) emitted when concrete type does not satisfy declared constraint

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend parser and AST for generic constraint syntax** - `25e89ed` (feat)
2. **Task 2: Add constraint checking in type checker at CALL and CONSTRUCT sites** - `35a7451` (feat)

## Files Created/Modified
- `src/parser/ast.h` - Added constraint_name field to Iron_Ident struct
- `src/parser/parser.c` - Parse optional constraint syntax in iron_parse_generic_params; initialize constraint_name=NULL on all ident allocations
- `src/analyzer/typecheck.c` - Added type_satisfies_constraint, check_generic_constraints helpers; hooked into CALL, call-as-construction, and CONSTRUCT cases

## Decisions Made
- constraint_name stored directly on Iron_Ident rather than a separate AST node -- keeps change minimal and avoids new node kind
- Constraint satisfaction uses dual check: nominal (object declares implements) OR structural (object has all interface methods) -- matches existing check_interface_completeness pattern
- Concrete type inference from call args: scan formal params for type annotation matching generic param name, use corresponding arg type
- Stack-allocated Iron_Type*[16] array for concrete types -- bounded, no heap allocation needed

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Initialize constraint_name on all Iron_Ident allocations**
- **Found during:** Task 1
- **Issue:** Arena allocator does not zero memory, so all Iron_Ident allocations outside iron_parse_generic_params would have uninitialized constraint_name
- **Fix:** Added id->constraint_name = NULL for identifier, self, and super ident allocations in parser.c
- **Files modified:** src/parser/parser.c
- **Verification:** Build succeeds with zero warnings, all tests pass
- **Committed in:** 25e89ed (Task 1 commit)

---

**Total deviations:** 1 auto-fixed (1 bug fix)
**Impact on plan:** Essential correctness fix -- uninitialized pointer would cause undefined behavior. No scope creep.

## Issues Encountered
None

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Generic constraint checking infrastructure complete
- Ready for Plan 02 (integration tests for constraint checking)
- All existing unit and integration tests pass (no regressions)

---
*Phase: 37-generic-constraint-checking*
*Completed: 2026-04-03*
