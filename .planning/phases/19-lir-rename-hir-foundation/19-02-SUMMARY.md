---
phase: 19-lir-rename-hir-foundation
plan: 02
subsystem: compiler
tags: [hir, ir, ast, compiler, c]

# Dependency graph
requires:
  - phase: 19-lir-rename-hir-foundation/19-01
    provides: LIR namespace (IronLIR_/IRON_LIR_/iron_lir_) established, HIR namespace clear
provides:
  - HIR data structures for all Iron language constructs (src/hir/hir.h, src/hir/hir.c)
  - 13 HIR statement kinds with tree-shaped structured control flow
  - 28 HIR expression kinds covering all Iron language expressions
  - Named variable system via VarId + name_table (IronHIR_VarId, iron_hir_alloc_var, iron_hir_var_name)
  - Unit tests verifying all HIR node construction (10 tests, all passing)
affects:
  - Phase 20 (HIR lowering/printing/verification)
  - Any plan building HIR-to-LIR lowering passes

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "HIR uses tree-shaped structured control flow (not CFG): if/while/for/match as stmt nodes"
    - "HIR VarId system: uint32_t VarId maps to name_table[id] for name and type lookup"
    - "HIR Module owns arena via malloc'd Iron_Arena*; sentinel at name_table[0] = index-by-id"
    - "HIR constructors follow LIR pattern: ARENA_ALLOC + memset(0) + field assignment + return"
    - "HIR IronHIR_BinOp / IronHIR_UnOp are HIR-level enums (not AST Iron_OpKind token values)"

key-files:
  created:
    - src/hir/hir.h
    - src/hir/hir.c
    - tests/hir/test_hir_data.c
    - tests/hir/CMakeLists.txt
  modified:
    - CMakeLists.txt

key-decisions:
  - "HIR Module owns a heap-allocated Iron_Arena* (malloc'd) so modules can be destroyed independently from any caller arena"
  - "IronHIR_BinOp and IronHIR_UnOp are HIR-native enums; AST Iron_OpKind (int/token) is not reused in HIR"
  - "name_table[0] holds the IRON_HIR_VAR_INVALID sentinel; VarId indexing is direct (no subtraction)"
  - "Closure params stored by value (IronHIR_Param array pointer, not stb_ds) to match constructors passing stack-allocated arrays in tests"

patterns-established:
  - "HIR constructors: ARENA_ALLOC(mod->arena, T) + memset(0) + field set + return"
  - "HIR block stmts use stb_ds arrput via iron_hir_block_add_stmt"
  - "HIR tests: global g_mod in setUp/tearDown; iron_types_init(NULL) for primitive singletons"

requirements-completed: [HIR-01, HIR-02, HIR-03, HIR-04]

# Metrics
duration: 10min
completed: 2026-03-30
---

# Phase 19 Plan 02: HIR Data Structures Summary

**HIR type system with 13 stmt kinds, 28 expr kinds, VarId named-variable table, arena-based constructors, and 10 unit tests covering all Iron language constructs**

## Performance

- **Duration:** 10 min
- **Started:** 2026-03-30T12:22:00Z
- **Completed:** 2026-03-30T12:31:56Z
- **Tasks:** 2
- **Files modified:** 5

## Accomplishments
- Created `src/hir/hir.h` with complete HIR type system: IronHIR_VarId, 13 StmtKind variants, 28 ExprKind variants, IronHIR_BinOp/UnOp, helper structs (MatchArm, Param, VarInfo, Block), full Stmt and Expr tagged unions, Func and Module structs, all constructor declarations
- Created `src/hir/hir.c` implementing all constructors: module create/destroy with heap-allocated arena, alloc_var with sentinel name_table, block/func management, 13 stmt constructors, 27 expr constructors
- Created `tests/hir/test_hir_data.c` with 10 unit tests covering all requirements (HIR-01 through HIR-04) — all pass

## Task Commits

Each task was committed atomically:

1. **Task 1: Create HIR type definitions, constructors, and CMake wiring** - `3a0e484` (feat)
2. **Task 2: Create HIR data structure unit tests** - `e524059` (feat)

## Files Created/Modified
- `src/hir/hir.h` - All HIR types, enums, structs, constructor declarations
- `src/hir/hir.c` - All HIR constructor implementations
- `tests/hir/test_hir_data.c` - 10 unit tests for HIR node construction
- `tests/hir/CMakeLists.txt` - test_hir_data target registered with "hir" label
- `CMakeLists.txt` - Added src/hir/hir.c to iron_compiler sources; added add_subdirectory(tests/hir)

## Decisions Made
- HIR Module allocates its own `Iron_Arena*` via `malloc` so modules can be destroyed independently from any caller arena; `iron_hir_module_destroy` calls `iron_arena_free` then `free`
- `IronHIR_BinOp` and `IronHIR_UnOp` are HIR-native enums (not reusing AST's `Iron_OpKind` int/token values), keeping HIR independent of the lexer
- `name_table[0]` holds the `IRON_HIR_VAR_INVALID` sentinel so `VarId` serves as a direct index with no offset arithmetic
- Closure params passed as stack-allocated `IronHIR_Param[]` pointer (not stb_ds arrput), consistent with the LIR param pattern

## Deviations from Plan
None - plan executed exactly as written.

## Issues Encountered
None.

## Next Phase Readiness
- HIR data layer is complete; src/hir/hir.h is the single include needed by Phase 20
- HIR printer and verifier can be added as separate plans using these constructors
- HIR-to-LIR lowering pass can begin using IronHIR_Module as input and IronLIR_Module as output

---
*Phase: 19-lir-rename-hir-foundation*
*Completed: 2026-03-30*
