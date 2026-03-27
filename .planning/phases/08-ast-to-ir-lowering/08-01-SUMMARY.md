---
phase: 08-ast-to-ir-lowering
plan: 01
subsystem: ir
tags: [ir, lowering, ssa, ast, codegen, poison, phi, short-circuit]

# Dependency graph
requires:
  - phase: 07-ir-foundation
    provides: IronIR_Module/Func/Block/Instr data structures, all instruction constructors, printer, verifier
provides:
  - IronIR_LowerCtx struct with all lowering state fields
  - iron_ir_lower() two-pass orchestration (Pass 1 declarations, Pass 2 bodies)
  - lower_expr() handling all 22+ expression node kinds
  - IRON_IR_POISON instruction kind for error placeholders
  - 400-range diagnostic error codes for lowering errors
  - lower_internal.h private header shared across lower_*.c files
  - test_ir_lower.c scaffold with snapshot infrastructure
affects:
  - 08-02 (lower_stmts.c uses lower_internal.h and LowerCtx)
  - 08-03 (lower_types.c uses lower_internal.h and LowerCtx)
  - 09-ir-to-c-emitter (uses IronIR_Module produced by iron_ir_lower)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Two-pass lowering: Pass 1 registers declarations, Pass 2 lowers function bodies
    - val_binding_map for immutable SSA values, var_alloca_map for mutable alloca+load+store
    - Short-circuit AND/OR use branch+phi CFG pattern (not IRON_IR_AND/OR instructions)
    - IRON_IR_POISON as diagnostic placeholder that verifier flags
    - emit_poison() emits both a diagnostic AND the poison instruction
    - Params lowered via synthetic ValueId + alloca+store so IDENT lookup works uniformly

key-files:
  created:
    - src/ir/lower_internal.h
    - src/ir/lower_exprs.c
    - tests/ir/test_ir_lower.c
    - tests/ir/snapshots/identity.expected
    - tests/ir/snapshots/arithmetic.expected
  modified:
    - src/ir/lower.c
    - src/ir/ir.h
    - src/ir/ir.c
    - src/ir/print.c
    - src/ir/verify.c
    - src/diagnostics/diagnostics.h
    - CMakeLists.txt

key-decisions:
  - "expr_type() helper extracts resolved_type by casting to Iron_ExprNode layout prefix — avoids code duplication since Iron_Node base does not have resolved_type"
  - "Iron_CallExpr has no func_decl field in AST — direct calls emit func_ref+func_ptr instead of passing func_decl pointer"
  - "Iron_Param has no declared_type — params resolved via fn->params[p].type set by lower_module_decls from type_ann"
  - "Params use alloca+load model (not val_binding_map) for uniformity — synthetic ValueId bumps fn->next_value_id, NULL in value_table"
  - "lower_stmt stub in lower.c handles VAL_DECL/VAR_DECL/ASSIGN/RETURN/DEFER/FREE — full impl deferred to Plan 08-02"

patterns-established:
  - "lower_expr() dispatch: cast to specific AST type, access typed fields, call IR constructor"
  - "Short-circuit boolean: branch on LHS, true/false block with phi at merge"
  - "lower_block(): push_defer_scope, lower stmts, emit_defers_ir, pop_defer_scope"
  - "IRON_IR_POISON: both emits diagnostic AND creates placeholder instruction"

requirements-completed: [INSTR-01, INSTR-02, INSTR-03, INSTR-04, INSTR-05, INSTR-06, INSTR-07, INSTR-08, INSTR-09, INSTR-10, INSTR-11, INSTR-12, INSTR-13]

# Metrics
duration: 11min
completed: 2026-03-27
---

# Phase 8 Plan 1: Lowering Infrastructure and Expression Pass Summary

**Two-pass AST-to-IR lowering with IronIR_LowerCtx, POISON instruction, and complete lower_expr() for all 22 expression kinds using alloca/val model and branch-based short-circuit booleans**

## Performance

- **Duration:** 11 min
- **Started:** 2026-03-27T14:38:18Z
- **Completed:** 2026-03-27T14:49:11Z
- **Tasks:** 4 (Task 0, 1, 2, 3)
- **Files modified:** 11

## Accomplishments

- IRON_IR_POISON instruction kind fully integrated: constructible, printable, verifier-detectable
- IronIR_LowerCtx struct defined with all lowering state (val/var maps, defer stack, loop context, pending lifts)
- iron_ir_lower() performs two-pass lowering: Pass 1 registers function signatures, Pass 2 lowers bodies
- lower_expr() handles all 22 AST expression kinds including short-circuit AND/OR with branch+phi CFG

## Task Commits

Each task was committed atomically:

1. **Task 0: Test scaffold** - `b896f42` (test)
2. **Task 1: IRON_IR_POISON and 400-range codes** - `81c4d80` (feat)
3. **Task 2: LowerCtx and two-pass orchestration** - `e4d6967` (feat)
4. **Task 3: lower_exprs.c complete** - `2ef36f8` (feat)

## Files Created/Modified

- `src/ir/lower_internal.h` - IronIR_LowerCtx struct, LiftPending, all shared helper declarations
- `src/ir/lower.c` - Full two-pass orchestration, helper implementations, lower_stmt stub
- `src/ir/lower_exprs.c` - Complete lower_expr() dispatch for all 22+ expression kinds
- `src/ir/ir.h` - Added IRON_IR_POISON enum value, union payload, and constructor declaration
- `src/ir/ir.c` - Added iron_ir_poison() constructor implementation
- `src/ir/print.c` - Added IRON_IR_POISON case printing "poison" with optional type
- `src/ir/verify.c` - IRON_IR_POISON in collect_operands (no operands); verifier flags poison as error
- `src/diagnostics/diagnostics.h` - 400-range codes: LOWER_UNSUPPORTED, LOWER_UNRESOLVED_IDENT, LOWER_INVALID_ASSIGN, LOWER_INVALID_MATCH
- `tests/ir/test_ir_lower.c` - Unity test scaffold with helpers and placeholder test
- `tests/ir/snapshots/` - identity.expected and arithmetic.expected placeholders
- `CMakeLists.txt` - Added test_ir_lower target and lower_exprs.c to iron_compiler

## Decisions Made

- `Iron_CallExpr` has no `func_decl` field in AST: direct calls emit `func_ref` + `func_ptr` rather than passing a func_decl pointer to iron_ir_call
- `Iron_Param` has no `declared_type`: param types resolved through `fn->params[p].type` set by lower_module_decls from the type annotation
- `Iron_Node` base struct has no `resolved_type`: each expression case casts to the specific type struct to access the field; `expr_type()` helper uses a common layout prefix for the default/error cases
- Params use alloca+load model for uniform IDENT resolution, with synthetic ValueIds (NULL in value_table) for the argument values

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed unused-function compiler errors in test scaffold**
- **Found during:** Task 0 (test scaffold creation)
- **Issue:** `-Werror,-Wunused-function` errors for helper functions not yet used by wave-0 tests
- **Fix:** Added `#pragma GCC diagnostic ignored "-Wunused-function"` around stub helpers
- **Files modified:** tests/ir/test_ir_lower.c
- **Verification:** Build succeeded, test passes
- **Committed in:** b896f42

**2. [Rule 1 - Bug] Fixed `base.span` field access errors**
- **Found during:** Task 2 (lower.c implementation)
- **Issue:** Plan used `vd->base.span` etc. but AST structs embed `span` directly as first field, not via `base`
- **Fix:** Changed all `->base.span` to `->span` and `->base.kind` to `->kind`
- **Files modified:** src/ir/lower.c
- **Verification:** Build succeeded
- **Committed in:** e4d6967

**3. [Rule 1 - Bug] Fixed `Iron_Param.declared_type` field access**
- **Found during:** Task 2 (lower_module_decls and lower_func_body)
- **Issue:** Plan referenced `ap->declared_type` but `Iron_Param` has only `type_ann` (a node pointer)
- **Fix:** Added `resolve_type_from_ann()` helper for Pass 1; Pass 2 uses `fn->params[p].type` set during Pass 1
- **Files modified:** src/ir/lower.c
- **Verification:** Build succeeded, all 27 tests pass
- **Committed in:** e4d6967

**4. [Rule 1 - Bug] Fixed `node->resolved_type` access on Iron_Node base**
- **Found during:** Task 3 (lower_exprs.c)
- **Issue:** `Iron_Node` base struct has no `resolved_type`; each expression subtype has it
- **Fix:** Cast to specific expression struct types; added `expr_type()` helper for fallback
- **Files modified:** src/ir/lower_exprs.c
- **Verification:** Build succeeded with zero errors
- **Committed in:** 2ef36f8

---

**Total deviations:** 4 auto-fixed (Rule 1 - bug corrections for field name mismatches)
**Impact on plan:** All fixes were for discrepancies between plan field names and actual AST struct definitions. No scope creep. All acceptance criteria met.

## Issues Encountered

The plan's interface descriptions used several field names that differ from the actual AST struct definitions (`base.span`, `declared_type` on params, `node->resolved_type`). All were caught at compile time and fixed inline following Rule 1.

## Next Phase Readiness

- lower_internal.h and IronIR_LowerCtx fully defined — Plans 08-02/08-03 can include and extend
- lower_stmt stub handles VAL_DECL, VAR_DECL, ASSIGN, RETURN, DEFER, FREE — Plan 08-02 replaces with lower_stmts.c
- lower_module_decls stub handles FUNC_DECL only — Plan 08-03 extends to objects/enums/externs
- All 27 tests pass — no regressions

---
*Phase: 08-ast-to-ir-lowering*
*Completed: 2026-03-27*

## Self-Check: PASSED

All created files verified present. All task commits verified in git log.
