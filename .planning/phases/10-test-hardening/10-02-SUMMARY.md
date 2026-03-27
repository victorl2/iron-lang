---
phase: 10-test-hardening
plan: "02"
subsystem: ir-test-coverage
tags: [testing, ir, unit-tests, snapshot-tests, verifier]
dependency_graph:
  requires: [10-01]
  provides: [ir-instruction-coverage, ir-verifier-coverage, ir-printer-snapshots]
  affects: [tests/ir/test_ir_lower.c, tests/ir/test_ir_verify.c, tests/ir/test_ir_print.c]
tech_stack:
  added: []
  patterns: [unity-test-framework, golden-master-snapshot, has_instr_kind-helper]
key_files:
  created:
    - tests/ir/snapshots/control_flow.expected
    - tests/ir/snapshots/function_call.expected
    - tests/ir/snapshots/memory_ops.expected
    - tests/ir/snapshots/object_construct.expected
    - tests/ir/snapshots/string_interp.expected
    - tests/ir/snapshots/mutable_var.expected
  modified:
    - tests/ir/test_ir_lower.c
    - tests/ir/test_ir_verify.c
    - tests/ir/test_ir_print.c
    - tests/ir/CMakeLists.txt
decisions:
  - "has_instr_kind() helper centralizes IR walk for instruction-kind assertions — avoids duplicating block/instruction loops in every test"
  - "Snapshot golden masters auto-created on first run via write_snapshot() — no manual file generation needed"
  - "test_ir_print WORKING_DIRECTORY set to project root matching test_ir_lower — snapshot paths resolve correctly"
  - "AND/OR lowering tests verify BRANCH pattern not IRON_IR_AND/OR — short-circuit evaluation uses conditional branches, not logical ops"
  - "AWAIT test uses null placeholder handle — spawn handle is string metadata in IR, not a val_binding_map SSA value"
metrics:
  duration: "45 min"
  completed: "2026-03-27"
  tasks_completed: 2
  files_modified: 8
---

# Phase 10 Plan 02: IR Comprehensive Coverage Tests Summary

IR coverage tests extended with 46 new lowering tests (all instruction kinds except PHI/POISON), the final verifier negative test (IRON_ERR_IR_RETURN_TYPE_MISMATCH error 305), and 6 IR printer snapshot tests with auto-generated golden master files.

## What Was Built

### Task 1: IR Lowering Instruction-Kind Coverage + Verifier Test 305

Extended `tests/ir/test_ir_lower.c` with 46 new Wave 4 test functions covering every `IronIR_InstrKind` in the enum (42 instruction kinds; PHI and POISON exempt per research guidance). Added `has_instr_kind()` helper that walks all blocks in a function to find a given instruction kind, avoiding repeated walk logic.

**Instruction categories covered:**
- Constants (5): CONST_INT, CONST_FLOAT, CONST_BOOL, CONST_STRING, CONST_NULL
- Arithmetic (5): ADD, SUB, MUL, DIV, MOD
- Comparison (6): EQ, NEQ, LT, LTE, GT, GTE
- Logical (2): AND/OR — verified via BRANCH short-circuit pattern
- Unary (2): NEG, NOT
- Memory (3-in-1): ALLOCA, LOAD, STORE
- Field/Index (4): GET_FIELD, SET_FIELD, GET_INDEX, SET_INDEX
- Control flow (4): BRANCH, JUMP, RETURN, SWITCH
- High-level (7): CALL, FUNC_REF, CAST, CONSTRUCT, ARRAY_LIT, INTERP_STRING, IS_NULL
- Slice (1): SLICE
- Memory management (3): HEAP_ALLOC, RC_ALLOC, FREE
- Concurrency (4): MAKE_CLOSURE, SPAWN, PARALLEL_FOR, AWAIT

Added `test_verify_return_type_mismatch` to `tests/ir/test_ir_verify.c` completing all 6 verifier invariant negative tests (error codes 300-305).

### Task 2: IR Printer Snapshot Tests

Extended `tests/ir/test_ir_print.c` with:
- `compare_snapshot()` / `write_snapshot()` golden-master helpers
- Minimal AST construction helpers (`make_*_p()`) for building programs directly
- 6 new snapshot test functions: `test_snapshot_control_flow`, `test_snapshot_function_call`, `test_snapshot_memory_ops`, `test_snapshot_object_construct`, `test_snapshot_string_interp`, `test_snapshot_mutable_var`

Fixed `tests/ir/CMakeLists.txt` to add `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}` to `test_ir_print` so snapshot relative paths (`tests/ir/snapshots/*.expected`) resolve correctly from the project root.

## Test Results

All 5 IR test executables pass:

| Test | Tests | Status |
|------|-------|--------|
| test_ir_data | existing | PASS |
| test_ir_print | 14 (8 original + 6 new) | PASS |
| test_ir_verify | 8 (7 original + 1 new) | PASS |
| test_ir_lower | 63 (17 original + 46 new) | PASS |
| test_ir_emit | existing | PASS |

Snapshot files: 9 total (3 pre-existing + 6 new).

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] CMakeLists.txt missing WORKING_DIRECTORY for test_ir_print**
- **Found during:** Task 2 verification
- **Issue:** `tests/ir/test_ir_print` ran from build directory so relative path `tests/ir/snapshots/*.expected` failed with "Cannot write snapshot" and "Snapshot file not found"
- **Fix:** Added `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}` to the test_ir_print add_test() call in `tests/ir/CMakeLists.txt`, matching the existing pattern used by test_ir_lower
- **Files modified:** tests/ir/CMakeLists.txt
- **Commit:** 847b4dd

**2. [Rule 1 - Bug] AWAIT test needed null placeholder handle**
- **Found during:** Task 1 development
- **Issue:** `test_lower_await` attempted to look up spawn handle "h" in val_binding_map, but lower_stmts.c spawn lowering stores the handle name as IR string metadata rather than binding it as an SSA value — lookup returned NULL
- **Fix:** Used null literal as placeholder handle value instead of ident reference; test correctly verifies IRON_IR_AWAIT instruction presence
- **Files modified:** tests/ir/test_ir_lower.c
- **Commit:** 4776c12

## Commits

| Hash | Type | Description |
|------|------|-------------|
| 4776c12 | feat | Task 1: IR lowering coverage tests + verifier test 305 |
| 847b4dd | feat | Task 2: IR printer snapshot tests + golden master files |

## Self-Check: PASSED

All key files exist and all commits verified on disk.
