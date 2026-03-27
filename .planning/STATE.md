---
gsd_state_version: 1.0
milestone: v1.1
milestone_name: High IR
status: executing
stopped_at: Completed 09-02-PLAN.md
last_updated: "2026-03-27T16:56:55.824Z"
last_activity: "2026-03-27 — Completed 07-01: IR data structure scaffold"
progress:
  total_phases: 5
  completed_phases: 3
  total_plans: 7
  completed_plans: 7
  percent: 10
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-27)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** Phase 7 — IR Foundation

## Current Position

Phase: 7 of 11 (IR Foundation)
Plan: 1 of 2 in current phase
Status: In progress
Last activity: 2026-03-27 — Completed 07-01: IR data structure scaffold

Progress: [#.........] 10%

## Performance Metrics

**Velocity:**
- Total plans completed: 1
- Average duration: 5 min
- Total execution time: 0.08 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 07-ir-foundation | 1/2 | 5 min | 5 min |
| Phase 07-ir-foundation P02 | 14 | 3 tasks | 7 files |
| Phase 08-ast-to-ir-lowering P01 | 11 | 4 tasks | 11 files |
| Phase 08-ast-to-ir-lowering P02 | 13 | 2 tasks | 8 files |
| Phase 08-ast-to-ir-lowering P03 | 45 | 3 tasks | 14 files |
| Phase 09-c-emission-and-cutover P01 | 45 | 2 tasks | 3 files |
| Phase 09-c-emission-and-cutover P02 | 35 | 2 tasks | 4 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- High IR before LLVM — decoupled IR enables backend swap without rewriting lowering logic
- SSA form for IR — enables future optimization passes and maps cleanly to LLVM IR
- Full codegen replacement — no dual-path maintenance burden; clean architectural break
- Alloca+load+store model for mutable variables — sidesteps phi-placement complexity
- Braun et al. 2013 SSA construction — single-pass incremental, no dominance frontiers needed
- Separate ir_arena from ast_arena — correct lifetime management as pipeline evolves
- free_instr union member name (not free) — avoids C stdlib name conflict in tagged union
- value_table[0] pre-seeded NULL — IRON_IR_VALUE_INVALID=0 never maps to a real instruction
- alloc_instr() static helper centralizes arena allocation, value numbering, and block appending
- [Phase 07-ir-foundation]: Printer uses temporary Iron_Arena for type_to_string calls — freed before return, no leak
- [Phase 07-ir-foundation]: Verifier collects all errors without early exit — reports all violations in one pass
- [Phase 07-ir-foundation]: collect_operands() helper centralizes ValueId extraction for all instruction kinds — reusable for future passes
- [Phase 08-ast-to-ir-lowering]: Iron_CallExpr has no func_decl field — direct calls emit func_ref+func_ptr instead of passing AST func_decl pointer
- [Phase 08-ast-to-ir-lowering]: Iron_Param has no declared_type — param types resolved via IrFunc.params[p].type set by lower_module_decls from type_ann
- [Phase 08-ast-to-ir-lowering]: Params use alloca+load model with synthetic ValueIds (NULL in value_table) for uniform IDENT resolution
- [Phase 08-ast-to-ir-lowering]: ctx->current_block = NULL after return for dead code suppression — avoids unterminated dead blocks that fail verifier
- [Phase 08-ast-to-ir-lowering]: IRON_TYPE_VOID return normalizes to NULL fn->return_type in lower_module_decls — aligns with verifier convention
- [Phase 08-ast-to-ir-lowering]: Arena-allocated params array in lower_module_decls — fixes use-after-free from stb_ds arrfree
- [Phase 08-ast-to-ir-lowering]: draw keyword removed from lexer entirely — becomes plain identifier enabling raylib.draw() naturally
- [Phase 08-ast-to-ir-lowering]: lower_types.c implements both Pass 1 and post-pass lifting; mono_registry populated lazily in Phase 9
- [Phase 08-ast-to-ir-lowering]: IR verifier skips is_extern=true functions — no structural verification needed for extern declarations
- [Phase 09-c-emission-and-cutover]: emit_type_to_c() implemented as static helper in emit_c.c — same type mapping as old codegen without Iron_Codegen dependency
- [Phase 09-c-emission-and-cutover]: phi_eliminate() uses make_alloca_instr() helpers to bypass alloc_instr() block requirement — safe in-place IR mutation before emission
- [Phase 09-c-emission-and-cutover]: Lifted functions detected by lambda_/spawn_/parallel_ name prefix — routed to lifted_funcs section before implementations
- [Phase 09-c-emission-and-cutover]: mangle_func_name() in emit_c.c applies Iron_ prefix to all user and builtin function names at emission time — IR stores names unmangled
- [Phase 09-c-emission-and-cutover]: FUNC_REF + CALL optimization: detect FUNC_REF in value_table to emit direct calls avoiding ISO-C-invalid variadic pointer cast
- [Phase 09-c-emission-and-cutover]: collect_captures() inlined as static function in lower_types.c — single consumer outside codegen, no need for shared header

### v1.0 Accumulated Context (preserved)

- stb_ds used throughout for hash maps and dynamic arrays
- Arena allocator is primary allocation strategy in compiler
- Codegen uses fprintf to emit C text directly
- Runtime library is separate CMake target (iron_runtime)
- Integration tests: iron build -> execute binary -> compare stdout
- Cross-platform: pthreads on all platforms (pthreads4w on Windows)

### Pending Todos

None yet.

### Blockers/Concerns

None yet.

## Session Continuity

Last session: 2026-03-27T16:56:55.822Z
Stopped at: Completed 09-02-PLAN.md
Resume file: None
