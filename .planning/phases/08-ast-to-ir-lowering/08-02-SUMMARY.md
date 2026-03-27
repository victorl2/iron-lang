---
phase: 08-ast-to-ir-lowering
plan: 02
subsystem: ir
tags: [ir, lowering, control-flow, ssa, statements, while, if-else, match, defer, for, return]

# Dependency graph
requires:
  - phase: 08-01
    provides: IronIR_LowerCtx, lower_internal.h, lower_exprs.c, iron_ir_lower() two-pass orchestration
provides:
  - lower_stmts.c with lower_stmt() for all statement node kinds
  - if/else/elif lowering producing conditional branch + then/else/merge blocks
  - while loop lowering producing header/body/exit blocks with back-edge jump
  - for loop lowering (sequential: pre_header/header/body/increment/exit CFG; parallel: IRON_IR_PARALLEL_FOR)
  - match lowering producing switch terminator with per-arm blocks
  - return lowering with defers-before-return and dead-code suppression
  - defer push onto scope stack; emitted at scope exit via emit_defers_ir
  - free/leak/spawn statement lowering
  - 11 unit tests + 3 snapshot golden master tests in test_ir_lower.c
affects:
  - 08-03 (lower_types.c uses same LowerCtx, same lower_block/lower_stmt infrastructure)
  - 09-ir-to-c-emitter (consumes IR with all control flow forms)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - merge_has_predecessor tracking for if-else: avoids dead merge blocks in all-return scenarios
    - ctx->current_block = NULL after return: dead code suppression without creating unterminated blocks
    - lower_block guards: early-exit when current_block == NULL (dead code after return)
    - Arena-allocated params array in lower_module_decls: avoids use-after-free from arrfree'd stb_ds array
    - Verifier synthetic-param exception: skip use-before-def for ValueIds <= param_count

key-files:
  created:
    - src/ir/lower_stmts.c
    - tests/ir/snapshots/if_else.expected
  modified:
    - src/ir/lower.c
    - src/ir/verify.c
    - tests/ir/test_ir_lower.c
    - tests/ir/snapshots/identity.expected
    - tests/ir/snapshots/arithmetic.expected
    - CMakeLists.txt

key-decisions:
  - "ctx->current_block = NULL after return (not dead-block pattern): avoids unterminated dead blocks that fail verifier"
  - "Void return type normalization: IRON_TYPE_VOID resolves to NULL fn->return_type so verifier convention is honored"
  - "merge_has_predecessor tracking in if-else: dead merge block (all branches return) gets void return terminator"
  - "Arena-allocated params array in lower_module_decls: fixes use-after-free from stb_ds arrfree"
  - "Verifier param-value exception: synthetic param ValueIds (1..param_count) are NULL in value_table by design"
  - "test_lower_if_else uses assign-fall-through pattern (not return-in-both-branches): valid IR with single merge-block return"

# Metrics
duration: 12min
completed: 2026-03-27
---

# Phase 8 Plan 2: Statement Lowering Pass Summary

**Complete lower_stmt() dispatch for all AST statement kinds: if/else/elif, while, for, match, return, defer, free, spawn, and nested blocks — producing verifier-clean SSA IR for all Iron control flow patterns**

## Performance

- **Duration:** 12 min
- **Started:** 2026-03-27T15:00:00Z
- **Completed:** 2026-03-27T15:12:00Z
- **Tasks:** 2
- **Files modified:** 8

## Accomplishments

- Created `src/ir/lower_stmts.c` (360 lines) with full `lower_stmt()` dispatch
- if/else/elif: conditional branch + then/else/merge blocks with predecessor tracking
- while loop: header/body/exit blocks with back-edge jump and loop context save/restore
- for loop: sequential pre_header/header/body/increment/exit CFG; parallel pushes LiftPending
- match: switch terminator with per-arm blocks and join block
- return: emits defers before return terminator, sets current_block=NULL for dead code suppression
- defer: pushes onto scope stack (deferred to emit_defers_ir at scope exit)
- 11 unit tests + 3 snapshot tests in test_ir_lower.c with golden master .expected files
- All 27 project tests pass with zero regressions

## Task Commits

1. **Task 1: lower_stmts.c** - `408256f` (feat)
2. **Task 2: tests and bug fixes** - `1826b8f` (feat)

## Files Created/Modified

- `src/ir/lower_stmts.c` — full lower_stmt() with all statement kinds
- `src/ir/lower.c` — removed stub lower_stmt, added dead-code guards, fixed params lifetime, void return normalization
- `src/ir/verify.c` — synthetic param ValueId exception for use-before-def check
- `tests/ir/test_ir_lower.c` — 11 unit tests + 3 snapshot tests
- `tests/ir/snapshots/identity.expected` — golden master IR for identity function
- `tests/ir/snapshots/arithmetic.expected` — golden master IR for arithmetic function
- `tests/ir/snapshots/if_else.expected` — golden master IR for if/else branching
- `CMakeLists.txt` — added lower_stmts.c to iron_compiler, WORKING_DIRECTORY for test_ir_lower

## Decisions Made

- `ctx->current_block = NULL` after return instead of creating dead blocks: avoids unterminated blocks that fail the verifier
- `IRON_TYPE_VOID` return type normalizes to `NULL fn->return_type` in `lower_module_decls` to align with verifier convention
- `merge_has_predecessor` tracking in if-else lowering: when all branches terminate, the merge block gets a dead void return terminator to satisfy the verifier's "every block needs terminator" invariant
- Arena-allocated params array in `lower_module_decls` to fix use-after-free (stb_ds dynamic array was freed while `fn->params` still pointed to it)
- Verifier treats ValueIds 1..param_count as valid synthetic param values (NULL in value_table by design from Plan 08-01)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed params lifetime use-after-free in lower_module_decls**
- **Found during:** Task 2 (tests ran with AddressSanitizer)
- **Issue:** `lower_module_decls` built params with stb_ds `arrput`, passed to `iron_ir_func_create`, then called `arrfree`. `fn->params` pointed to freed stb_ds memory. `lower_func_body` then read `fn->params[p].type` causing heap-use-after-free.
- **Fix:** Replaced stb_ds array with arena-allocated array in `lower_module_decls`
- **Files modified:** `src/ir/lower.c`
- **Commit:** 1826b8f

**2. [Rule 1 - Bug] Fixed void return type mismatch causing verifier failures**
- **Found during:** Task 2 (test_lower_empty_func verify failed)
- **Issue:** `lower_module_decls` passed `IRON_TYPE_VOID` (non-NULL) to `iron_ir_func_create`. Verifier uses `fn->return_type == NULL` to detect void functions. Implicit void returns with `is_void=true` failed "return type mismatch" check.
- **Fix:** Normalize VOID typed return to NULL in `lower_module_decls` before creating IronIR_Func
- **Files modified:** `src/ir/lower.c`
- **Commit:** 1826b8f

**3. [Rule 1 - Bug] Fixed use-after-free verifier error for synthetic param ValueIds**
- **Found during:** Task 2 (verify reported "use of undefined value %1" for parameterized functions)
- **Issue:** Synthetic param ValueIds (1..param_count) are intentionally NULL in value_table (design from Plan 08-01). The verifier's use-before-def check treated NULL value_table entries as errors unconditionally.
- **Fix:** Added exception in `verify.c`: skip use-before-def check for `1 <= op <= fn->param_count`
- **Files modified:** `src/ir/verify.c`
- **Commit:** 1826b8f

**4. [Rule 1 - Bug] Fixed dead code after return creating unterminated blocks**
- **Found during:** Task 2 (verifier reported "block 'return.dead' has no terminator")
- **Issue:** IRON_NODE_RETURN created a `return.dead` block and switched to it. For non-void functions, `lower_func_body`'s implicit-void-return logic (now void-only) left the dead block without a terminator. Verifier flagged this.
- **Fix:** Set `ctx->current_block = NULL` after RETURN instead of creating dead blocks. Added NULL guards in `lower_block` and `lower_func_body` body loops to stop lowering dead code.
- **Files modified:** `src/ir/lower_stmts.c`, `src/ir/lower.c`
- **Commit:** 1826b8f

**5. [Rule 2 - Missing Critical] Added WORKING_DIRECTORY to test_ir_lower ctest registration**
- **Found during:** Task 2 (ctest failed with "Snapshot file not found" — relative paths resolved from build dir)
- **Issue:** Snapshot tests used relative paths like `tests/ir/snapshots/...` which resolve correctly when running the binary from the project root but fail when ctest runs from the build directory.
- **Fix:** Added `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}` to `add_test` for test_ir_lower
- **Files modified:** `CMakeLists.txt`
- **Commit:** 1826b8f

---

**Total deviations:** 5 auto-fixed (Rules 1 and 2)
**Impact on plan:** All fixes were for discrepancies between plan assumptions and actual IR/verifier conventions established in Plan 08-01. No scope creep. All acceptance criteria met.

## Next Phase Readiness

- `lower_stmt()` fully implemented in `lower_stmts.c` — Plan 08-03 can focus on type declarations and extern registration
- Dead code suppression via NULL current_block is robust — no "missing terminator" verifier errors
- All 27 tests pass — no regressions to existing codegen or other passes

## Self-Check: PASSED
