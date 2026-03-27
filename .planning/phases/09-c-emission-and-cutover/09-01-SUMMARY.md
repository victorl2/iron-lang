---
phase: 09-c-emission-and-cutover
plan: 01
subsystem: compiler-backend
tags: [ir, codegen, c-emission, ssa, phi-elimination, strbuf]

# Dependency graph
requires:
  - phase: 08-ast-to-ir-lowering
    provides: IronIR_Module populated by iron_ir_lower()
  - phase: 07-ir-foundation
    provides: IR data structures, constructors, printer, verifier
provides:
  - Full iron_ir_emit_c() implementation (src/ir/emit_c.c, 1513 lines)
  - phi_eliminate() pre-pass (converts phi -> alloca+store+load)
  - emit_type_decls() with topological sort, forward decls, type tags, vtables
  - emit_func_body() with goto-based control flow and _vN value naming
  - Unit tests for all major emit_c behaviors (tests/ir/test_ir_emit.c)
affects: [09-02-cutover, integration-tests]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - Section-based StrBuf emission (9 sections: includes, forward_decls, struct_bodies, enum_defs, global_consts, prototypes, lifted_funcs, implementations, main_wrapper)
    - Value naming scheme: _v{id} — short, collision-free, derived from IronIR_ValueId
    - Phi elimination via in-place IR mutation before emission (alloca+store+load pattern)
    - Topological sort for struct body ordering (DFS with GRAY/BLACK coloring on type_decls)
    - Lifted functions (lambda_/spawn_/parallel_) routed to lifted_funcs section before implementations

key-files:
  created:
    - tests/ir/test_ir_emit.c
  modified:
    - src/ir/emit_c.c
    - CMakeLists.txt

key-decisions:
  - "emit_type_to_c() implemented as static helper in emit_c.c without Iron_Codegen dependency — same mapping as old codegen, no coupling to AST codegen structs"
  - "make_alloca_instr()/make_store_instr() helpers bypass alloc_instr() to create instructions without appending to a block, enabling safe phi_eliminate() insertion"
  - "Phi elimination modifies IR in-place — IR is consumed by emitter and freed after emission, so mutation is safe"
  - "is_lifted_func() detects lambda_/spawn_/parallel_ name prefixes to route function bodies to lifted_funcs section"

patterns-established:
  - "Block labels emitted as C labels: label:;\\n — semicolon required by C for label-before-closing-brace edge case"
  - "All IR value IDs emit as _v{id} C variables; alloca emits plain declaration, load copies from alloca var"
  - "Type tags emitted as #define IRON_TAG_Iron_X N immediately after struct body"

requirements-completed: [EMIT-01]

# Metrics
duration: 45min
completed: 2026-03-27
---

# Phase 9 Plan 01: C Emission Backend Summary

**IR-to-C emission backend with phi elimination, topological struct ordering, goto-based control flow, and 50 instruction cases covering all IronIR_InstrKind values**

## Performance

- **Duration:** ~45 min
- **Started:** 2026-03-27T16:00:00Z
- **Completed:** 2026-03-27T16:45:00Z
- **Tasks:** 2
- **Files modified:** 3 (src/ir/emit_c.c, tests/ir/test_ir_emit.c, CMakeLists.txt)

## Accomplishments

- Replaced the NULL stub in emit_c.c with a 1513-line full implementation
- phi_eliminate() pre-pass correctly rewrites phi nodes to alloca+store+load before emission
- Type declaration emission with topological sort, type tags (#define IRON_TAG_*), and interface vtable structs
- Function body emission with labeled goto-based C control flow and _vN value naming
- 6 unit tests covering all core emission scenarios — all pass via ctest

## Task Commits

1. **Task 1: Implement iron_ir_emit_c()** - `e41ce4d` (feat)
2. **Task 2: Create unit tests and register in CMake** - `e7cb697` (test)

## Files Created/Modified

- `src/ir/emit_c.c` — Full IR-to-C emission backend (1513 lines, replaces 6-line stub)
- `tests/ir/test_ir_emit.c` — 6 Unity tests for emit_c (hello world, arithmetic, control flow, alloca/load/store, type decl, phi elimination)
- `CMakeLists.txt` — Added test_ir_emit executable and ctest registration

## Decisions Made

- `emit_type_to_c()` implemented as a local static helper without `Iron_Codegen` dependency — avoids coupling between IR backend and AST codegen while producing identical type strings
- Phi elimination uses `make_alloca_instr()` / `make_store_instr()` helpers that directly allocate instructions via arena without appending to a block — this is necessary because `alloc_instr()` in ir.c requires a non-NULL block
- `is_lifted_func()` checks for `lambda_`, `spawn_`, and `parallel_` name prefixes to route function bodies to the `lifted_funcs` section (appears before `implementations` in concatenation order)

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] phi_eliminate() NULL block crash fix**
- **Found during:** Task 1 verification (test_emit_phi_elimination test)
- **Issue:** Initial phi_eliminate() called `iron_ir_alloca(fn, NULL, ...)` with a NULL block parameter. The underlying `alloc_instr()` in ir.c dereferences the block pointer unconditionally at line 28, causing a SIGSEGV under ASan.
- **Fix:** Replaced `iron_ir_alloca(fn, NULL, ...)` with `make_alloca_instr()` / `make_store_instr()` helpers that allocate directly via arena and assign a ValueId without touching any block. Then manually insert into the entry block and predecessor blocks respectively.
- **Files modified:** src/ir/emit_c.c
- **Verification:** `ctest -R test_ir_emit` — all 6 tests pass including test_emit_phi_elimination
- **Committed in:** e41ce4d (Task 1 commit, fix applied before final commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 — bug in phi elimination)
**Impact on plan:** The fix was essential for correctness. The alloca insertion logic now correctly works without requiring a live block pointer at creation time.

## Issues Encountered

None beyond the phi elimination NULL-block crash documented above.

## User Setup Required

None — no external services or environment variables required.

## Next Phase Readiness

- `iron_ir_emit_c()` is ready to be wired into `src/cli/build.c` as the new codegen backend
- Plan 09-02 can proceed: swap `build.c` to call `iron_ir_lower()` + `iron_ir_emit_c()` instead of `iron_codegen()`
- After integration tests pass, Plan 09-03 can delete `src/codegen/` entirely

## Self-Check: PASSED

- src/ir/emit_c.c: FOUND
- tests/ir/test_ir_emit.c: FOUND
- 09-01-SUMMARY.md: FOUND
- Commit e41ce4d: FOUND
- Commit e7cb697: FOUND

---
*Phase: 09-c-emission-and-cutover*
*Completed: 2026-03-27*
