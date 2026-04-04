---
phase: 36-methods-on-enums-and-syntax-migration
plan: 01
subsystem: compiler
tags: [iron-lang, enum, methods, adt, hir, lir, typecheck, resolver]

# Dependency graph
requires:
  - phase: 34-hir-extensions-and-match-lowering
    provides: HIR match lowering, ADT tag-based SWITCH emission, pattern binding injection
  - phase: 33-typecheck
    provides: variant_payload_types, exhaustiveness checking, enum type resolution
provides:
  - Methods on enum types via `func EnumType.method()` syntax
  - self in enum methods resolves to enum value and is usable as match scrutinee
  - Plain enum (no payload) match lowering using IRON_HIR_EXPR_PATTERN in non-ADT path
affects: [37-generic-enums, 38-recursive-enums]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Enum method guard relaxation: check sym_kind != IRON_SYM_TYPE && sym_kind != IRON_SYM_ENUM"
    - "HIR self-type for enum methods: else-if IRON_NODE_ENUM_DECL in self-type lookup loop"
    - "LIR method mangling for enums: else-if IRON_TYPE_ENUM in obj_type check"
    - "Plain enum match: IRON_HIR_EXPR_PATTERN handled in non-ADT match path alongside INT_LIT"

key-files:
  created:
    - tests/integration/adt_enum_method.iron
    - tests/integration/adt_enum_method.expected
    - tests/integration/adt_plain_enum_method.iron
    - tests/integration/adt_plain_enum_method.expected
  modified:
    - src/analyzer/resolve.c
    - src/hir/hir_lower.c
    - src/hir/hir_to_lir.c
    - src/analyzer/typecheck.c

key-decisions:
  - "Plain enum match lowering fixed in non-ADT SWITCH path: IRON_HIR_EXPR_PATTERN (not IRON_HIR_EXPR_ENUM_CONSTRUCT) is the correct kind for all variant patterns including unit variants"
  - "Bool (not bool) is the correct type name in iron-lang — lowercase bool is not a recognized type"

patterns-established:
  - "When enabling a feature for enums alongside objects, all four sites need updating: resolver guard, HIR self-type, LIR type-name mangling, typecheck return type"

requirements-completed: [EMETH-01, EMETH-02, MATCH-01, MATCH-07]

# Metrics
duration: 20min
completed: 2026-04-04
---

# Phase 36 Plan 01: Enum Methods Summary

**Four targeted guard relaxations enable `func EnumType.method()` syntax; also fixes plain enum match lowering for IRON_HIR_EXPR_PATTERN in non-ADT SWITCH path**

## Performance

- **Duration:** ~20 min
- **Started:** 2026-04-04T13:00:00Z
- **Completed:** 2026-04-04T13:14:36Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments

- Methods on ADT enums (payloads): `func Shape.area() -> Int` compiles and runs; `match self` inside body destructures variants correctly (25, 12)
- Methods on plain enums (no payloads): `func Dir.is_vertical() -> Bool` compiles and runs; `match self` dispatches by variant index correctly (true, false)
- Fixed pre-existing bug in plain enum match lowering: IRON_HIR_EXPR_PATTERN arms were falling to default in the non-ADT switch path
- All 184 integration tests pass with zero regressions

## Task Commits

1. **Task 1: Create integration test files for enum methods** - `872ce8e` (test)
2. **Task 2: Apply four targeted code changes to enable enum methods** - `ee86f7d` (feat)

## Files Created/Modified

- `tests/integration/adt_enum_method.iron` - ADT enum method test with match on self
- `tests/integration/adt_enum_method.expected` - expects 25 then 12
- `tests/integration/adt_plain_enum_method.iron` - plain enum method test, Bool return type
- `tests/integration/adt_plain_enum_method.expected` - expects true then false
- `src/analyzer/resolve.c` - attach_method guard relaxed to accept IRON_SYM_ENUM
- `src/hir/hir_lower.c` - self-type lookup added else-if for IRON_NODE_ENUM_DECL
- `src/hir/hir_to_lir.c` - type-name extraction for IRON_TYPE_ENUM added; plain enum match IRON_HIR_EXPR_PATTERN handled in non-ADT path
- `src/analyzer/typecheck.c` - method call return type resolution added for IRON_TYPE_ENUM receiver

## Decisions Made

- Plain enum match lowering bug: the non-ADT match path only handled `IRON_HIR_EXPR_INT_LIT` patterns. Plain enum variant patterns are `IRON_HIR_EXPR_PATTERN` (same as ADT). Fixed by adding an `else if` branch in the non-ADT path that resolves the variant index by name from the enum decl.
- Bool capitalization: iron-lang uses `Bool` (uppercase) not `bool` — test file corrected.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed plain enum match lowering in non-ADT SWITCH path**
- **Found during:** Task 2 (verification of adt_plain_enum_method)
- **Issue:** Plain enum match (`Dir.North -> return true`) produced `false false` instead of `true false`. The non-ADT switch path only checked `IRON_HIR_EXPR_INT_LIT` patterns; `IRON_HIR_EXPR_PATTERN` arms (which represent ALL enum variant patterns including unit variants) fell through to the default arm.
- **Fix:** Added `else if (arm->pattern->kind == IRON_HIR_EXPR_PATTERN && match_ed)` branch in the non-ADT path that resolves the variant index by name and adds it to the case table, matching the behavior of the ADT path.
- **Files modified:** `src/hir/hir_to_lir.c`
- **Verification:** `./tmp/adt_plain_enum_method` outputs `true\nfalse` as expected
- **Committed in:** `ee86f7d` (Task 2 commit)

**2. [Rule 1 - Bug] Fixed Bool capitalization in test file**
- **Found during:** Task 2 (initial test run)
- **Issue:** Test file used `bool` (lowercase) which is not a recognized type in iron-lang; error `unknown type 'bool'`
- **Fix:** Changed `-> bool` to `-> Bool` in adt_plain_enum_method.iron
- **Files modified:** `tests/integration/adt_plain_enum_method.iron`
- **Verification:** Compiler accepts the type; file already committed in Task 2 commit
- **Committed in:** `ee86f7d` (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs — Rule 1)
**Impact on plan:** Both fixes necessary for correctness. The plain enum match fix resolves a gap in the existing match lowering that was previously untested.

## Issues Encountered

None beyond the auto-fixed deviations above.

## Next Phase Readiness

- EMETH-01 and EMETH-02 complete: enum methods compile and run end-to-end
- Plain enum match in a method body (`match self`) now works correctly
- Ready for Phase 36 Plan 02 (syntax migration if any remaining plans)

---
*Phase: 36-methods-on-enums-and-syntax-migration*
*Completed: 2026-04-04*
