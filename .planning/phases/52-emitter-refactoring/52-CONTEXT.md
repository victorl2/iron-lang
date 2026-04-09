# Phase 52: Emitter Refactoring - Context

**Gathered:** 2026-04-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Decompose the monolithic `emit_c.c` (~6500+ lines) into focused sub-modules with clean APIs. Zero behavioral changes — all existing integration tests must produce identical output. This is a pure refactoring phase. Pre-scan passes (chain detection, split collection detection, monomorphic detection) stay in emit_c.c; sub-modules handle specialized emission only.

</domain>

<decisions>
## Implementation Decisions

### Module boundaries
- **Core stays in emit_c.c** — `iron_lir_emit_c()`, `emit_func_body()`, `emit_instr()`, and all pre-scan passes remain in emit_c.c. Sub-modules handle specialized emission.
- **Pre-scans stay centralized** — all pre-scan passes (chain detection, split collection detection, monomorphic detection, etc.) stay together in `emit_func_body()` since they share state. Sub-modules receive pre-computed data.
- **Shared helpers in emit_helpers.c** — common functions used across modules (`emit_mangle_name`, `emit_val`, `emit_indent`, `emit_type_to_c`, `annotation_to_c`, `resolve_func_c_name`) extracted into `emit_helpers.c/h`. Avoids duplication and circular dependencies.
- **4 new files total**: `emit_helpers.c/h`, `emit_split.c/h`, `emit_fusion.c/h`, `emit_structs.c/h`

### EmitCtx ownership
- **Pass EmitCtx pointer** — EmitCtx stays as a single centralized struct. All sub-module functions take `EmitCtx *ctx` as first parameter. Simple, no boilerplate.
- **EmitCtx defined in emit_helpers.h** — full struct definition in the shared header so all sub-modules can access fields directly. No opaque pointer gymnastics.

### Header organization
- **One header per module** — `emit_split.h`, `emit_fusion.h`, `emit_structs.h`, `emit_helpers.h`. Each declares only that module's public functions. `emit_c.h` remains the external API (`iron_lir_emit_c`).
- Internal helpers within each module stay `static` — not exposed via header.

### Migration strategy
- **One module at a time** — Extract `emit_helpers` first (shared dependencies), then `emit_structs`, then `emit_split`, then `emit_fusion`. Run full test suite after each extraction. Safest approach — if something breaks, the diff is small.
- Each extraction is one atomic commit.

### Claude's Discretion
- Exact function grouping within each module (which specific functions go where)
- Whether to rename any internal functions for clarity during the move
- EmitCtx field documentation format (comments vs doc block)
- How to handle the `emit_ctx_cleanup()` function (in emit_helpers or emit_c)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Emitter code
- `src/lir/emit_c.c` — The monolithic file being refactored. Read in full to understand function dependencies.
- `src/lir/emit_c.h` — Current public API (`iron_lir_emit_c`). Stays as external interface.

### Analysis modules (pattern to follow)
- `src/lir/layout_analysis.h` — Example of a well-structured analysis module with clean API (Phase 48)
- `src/lir/value_range.h` — Another analysis module following same pattern (Phase 50)

### Build system
- `CMakeLists.txt` — Must add new .c files to `iron_compiler` target

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **EmitCtx struct** (~line 70-130 in emit_c.c): Central state with ~20+ fields. Will move to emit_helpers.h.
- **Helper functions** (~line 135-300): `emit_mangle_name`, `emit_val`, `emit_indent`, `emit_type_to_c`, `annotation_to_c`. Natural first extraction target.
- **IrTopoState and topo sort** (~line 4750-4810): Used only for struct ordering. Goes to emit_structs.c.

### Established Patterns
- **Module pattern from layout_analysis.h/c**: Header declares types + API, .c implements with static helpers. Same pattern for each emitter sub-module.
- **stb_ds usage**: All hash maps use stb_ds — sub-modules include `vendor/stb_ds.h` directly.

### Integration Points
- **emit_c.c calls sub-modules**: After refactoring, emit_c.c includes all 4 sub-module headers and calls their functions.
- **Sub-modules call emit_helpers**: Shared helpers are the foundation layer.
- **CMakeLists.txt**: 4 new .c files added to the `iron_compiler` library target.

</code_context>

<specifics>
## Specific Ideas

- Extraction order matters: helpers first (no deps), structs second (depends on helpers), split third (depends on helpers + structs for Stor types), fusion last (depends on helpers + may reference split collection types)
- The `emit_ctx_cleanup()` function should collect ALL hash map frees currently scattered at the end of `iron_lir_emit_c` — single point of cleanup
- Each sub-module's header should have a clear "what this module does" comment at the top

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 52-emitter-refactoring*
*Context gathered: 2026-04-08*
