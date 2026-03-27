# Phase 7: IR Foundation - Context

**Gathered:** 2026-03-27
**Status:** Ready for planning

<domain>
## Phase Boundary

Build the complete IR data structure scaffold — IrModule, IrFunc, IrBlock, IrInstr with every instruction kind — plus a human-readable printer and structural verifier. No lowering code, no C emission code. This phase produces the "schema" that Phase 8 (lowering) and Phase 9 (emission) consume.

</domain>

<decisions>
## Implementation Decisions

### IR Text Format (printer output)
- LLVM-style syntax: `%0 = add %1, %2 : Int`
- Full Iron type names in signatures and instructions (Int, Float, Bool, Player, List[Int], heap Player, rc Enemy, Player?) — NOT C-mapped types
- No raw pointer syntax (`*T`) — Iron doesn't have pointers; use `heap T`, `rc T`
- Named basic blocks derived from source: `entry`, `if.then`, `if.else`, `while.header`, `while.body`, `for_header`, `for_body`, `for_exit`, `base_case`, `recurse`
- Function signatures: `func @Iron_main(x: Int, y: Float) -> Bool { ... }`
- Module-level declarations section at top: types, externs, globals, then functions
- Annotations on by default (auto_free flags, escapes flags, [deferred] markers, source spans as trailing comments)
- Optional `--no-annotations` flag to hide all annotations for clean output

### Instruction Set Design
- Single flat `IrInstrKind` enum with ~40 variants, grouped by comment sections (constants, arithmetic, comparison, unary, memory, control flow, high-level)
- Per-kind structs inside a tagged union — each instruction kind gets its own named struct in the union (not a shared ops[3] array)
- Common fields on every instruction: kind, id (IrValueId), type (Iron_Type*), span (Iron_Span)
- High-level constructs kept as single opcodes: IR_MAKE_CLOSURE, IR_SPAWN, IR_PARALLEL_FOR, IR_AWAIT, IR_INTERP_STRING, IR_HEAP_ALLOC, IR_RC_ALLOC, IR_CONSTRUCT — C emitter handles expansion
- IR_SWITCH terminator for match/enum (enum variants mapped to integer discriminants)
- For loops lowered to CFG blocks (header/body/exit with counter via alloca), NOT a high-level for instruction
- Parallel-for stays as single IR_PARALLEL_FOR opcode (high-level, emitter expands)

### Naming Conventions
- Type prefix: `IronIR_` — IronIR_Module, IronIR_Func, IronIR_Block, IronIR_Instr, IronIR_ValueId, IronIR_BlockId
- Enum prefix: `IRON_IR_` — IRON_IR_CONST_INT, IRON_IR_ADD, IRON_IR_BRANCH, IRON_IR_CALL
- Function prefix: `iron_ir_` — iron_ir_module_create(), iron_ir_print(), iron_ir_verify(), iron_ir_lower()
- Matches existing codebase convention: Iron_ for types, IRON_ for enums, iron_ for functions

### File Organization
- Split by concern with separate .h/.c pairs:
  - `src/ir/ir.h` + `ir.c` — all data structures and constructors
  - `src/ir/lower.h` + `lower.c` — AST to IR (Phase 8, empty stubs in Phase 7)
  - `src/ir/emit_c.h` + `emit_c.c` — IR to C text (Phase 9, empty stubs in Phase 7)
  - `src/ir/print.h` + `print.c` — human-readable IR printer
  - `src/ir/verify.h` + `verify.c` — structural verifier
- Tests in `tests/ir/` subdirectory with separate files per concern

### Claude's Discretion
- Exact set of ~40 instruction kind variants (Claude determines the full enum from requirements)
- IrValueId and IrBlockId integer width (uint32_t vs uint16_t)
- Arena allocation strategy details (dedicated ir_arena sizing, growth policy)
- Verifier error message format and reporting mechanism
- Helper macros for common instruction creation patterns

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### IR Design
- `.planning/research/STACK.md` — Braun SSA construction algorithm, data structure recommendations, memory management strategy
- `.planning/research/FEATURES.md` — Complete instruction set mapping from AST nodes to IR instructions, feature dependencies, anti-features
- `.planning/research/ARCHITECTURE.md` — Module layout, pipeline integration, data flow diagrams
- `.planning/research/PITFALLS.md` — Day-one structural decisions, critical edge splitting, arena lifetime issues
- `.planning/research/SUMMARY.md` — Synthesized findings and phase ordering rationale

### Existing Codebase
- `src/codegen/codegen.h` — Current Iron_Codegen struct showing what state the IR module must carry (mono_registry, lambda_counter, defer_stacks, etc.)
- `src/parser/ast.h` — All Iron_NodeKind variants that the IR instruction set must cover
- `src/analyzer/types.h` — Iron_Type system that IR reuses directly (Iron_Type*, iron_type_to_string, type constructors)
- `src/util/arena.h` — Iron_Arena API that IR allocations use (iron_arena_alloc, ARENA_ALLOC macro)
- `src/diagnostics/diagnostics.h` — Iron_Span struct and Iron_DiagList for verifier error reporting

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Iron_Arena` (src/util/arena.h): Bump allocator with realloc growth — IR nodes arena-allocated from a separate ir_arena
- `ARENA_ALLOC(arena, T)` macro: Typed allocation helper — use for all IronIR_ struct allocation
- `Iron_Type*` system (src/analyzer/types.h): Full type hierarchy reused directly — no IR type wrappers
- `Iron_Span` (src/diagnostics/diagnostics.h): Source location struct — carried on every IronIR_Instr
- `Iron_DiagList` (src/diagnostics/diagnostics.h): Diagnostic collection — verifier reports errors through this
- `stb_ds.h` (src/vendor/stb_ds.h): Dynamic arrays and hash maps — used for instruction lists, block lists, mono registry
- `Iron_StrBuf` (src/util/strbuf.h): String buffer for building output — printer uses this

### Established Patterns
- Tagged unions with kind enum: AST uses Iron_NodeKind + union of per-kind structs — IR mirrors this pattern with IronIR_InstrKind
- Arena ownership: Each pipeline stage owns its arena (lexer arena, parser arena, etc.) — IR gets its own ir_arena
- stb_ds for collections: Dynamic arrays via arrput/arrlen, hash maps via shput/shget — IR uses same patterns
- Unity test framework: All C unit tests use Unity assertions — IR tests follow same pattern

### Integration Points
- `iron_codegen()` in src/codegen/codegen.h — entry point that IR pipeline will eventually replace (Phase 9)
- `iron_analyze()` in src/analyzer/analyzer.h — produces annotated AST that IR lowering consumes (Phase 8)
- `build.c` in src/cli/ — calls iron_codegen(); will switch to iron_ir_lower() + iron_ir_emit_c() (Phase 9)
- `CMakeLists.txt` — needs new src/ir/ sources added to iron_compiler target

</code_context>

<specifics>
## Specific Ideas

- IR print format should mirror LLVM IR feel: `%0 = add %1, %2 : Int` with `func @name() -> Type { ... }` blocks
- Module header shows full type/extern declarations before function bodies (like a C header section)
- Annotations (auto_free, escapes, [deferred], source spans) visible by default, toggled off with `--no-annotations`
- Named blocks for readability: `entry`, `if.then`, `while.body` — not numbered bb0, bb1

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 07-ir-foundation*
*Context gathered: 2026-03-27*
