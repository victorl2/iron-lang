# Phase 32: LIR Verifier Hardening - Context

**Gathered:** 2026-04-02
**Status:** Ready for planning

<domain>
## Phase Boundary

Add PHI type consistency checks and call argument type/count validation to the LIR verifier (`src/lir/verify.c`). This is an internal compiler safety net — catches lowering bugs before they produce invalid C.

</domain>

<decisions>
## Implementation Decisions

### PHI Type Checks
- Add as new invariant (Invariant 7) in the existing `verify_func` loop
- For each PHI instruction, look up each incoming value via `fn->value_table[value_id]` and check its `->type` matches the PHI's result `->type`
- Use `iron_type_equals()` for strict comparison (same as return type checks at Invariant 6)
- Emit `IRON_ERR_LIR_PHI_TYPE_MISMATCH` (new error code 306)

### Call Argument Validation
- Add as new invariant (Invariant 8) in the same loop
- For direct calls (`func_decl != NULL`): check `arg_count == func_decl->param_count` and each arg type matches corresponding param type
- Use `iron_type_equals()` for type comparison
- Emit `IRON_ERR_LIR_CALL_TYPE_MISMATCH` (new error code 307) for both count and type mismatches

### Indirect Calls
- Indirect calls (`func_decl == NULL`, uses `func_ptr`) are skipped for now — no function type signature is carried at LIR level
- This is tracked as v2 requirement AGEN-01

### Error Messages
- Follow existing pattern: `snprintf` into stack buffer, include function name and block label for location
- PHI: "PHI node type mismatch: incoming value %%N has type X, expected Y"
- Call: "call argument type mismatch in function 'name': argument N has type X, expected Y"
- Call count: "call argument count mismatch in function 'name': got N, expected M"

### Claude's Discretion
- Whether to add a helper function for type comparison or inline it
- Exact error message wording
- Whether to check all mismatches or stop at first per instruction

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### LIR Verifier
- `src/lir/verify.c` — Existing verifier with 6 invariants; add new checks here
- `src/lir/verify.h` — Public API (no changes needed)
- `src/lir/lir.h` — LIR instruction structs: PHI at line ~195 (phi.values/phi.count), CALL at line ~163 (call.func_decl/call.args/call.arg_count)

### Type System
- `src/analyzer/types.h` — `iron_type_equals()` declaration
- `src/analyzer/types.c` — `iron_type_equals()` implementation

### Diagnostics
- `src/diagnostics/diagnostics.h` — Error code definitions (300-305 used, add 306-307)

### Gaps Document
- `docs/semantic_analysis_gaps.md` §2 (PHI Node Type Consistency) and §3 (Call Argument Types in LIR)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `iron_type_equals()`: Already used in Invariant 6 (return type check) — reuse for PHI and call checks
- `collect_operands()`: Already collects PHI values and call args — but type checks need to go beyond just collecting IDs
- `fn->value_table[id]->type`: Pattern for looking up a value's type already used in return type check (line 450-456)

### Established Patterns
- All invariant checks follow the same structure: iterate blocks, iterate instrs, switch on kind, emit via `iron_diag_emit`
- Error codes are sequential `#define`s in `diagnostics.h`
- Messages use `snprintf` into 256-byte stack buffer
- Unreachable blocks are already skipped (BFS reachability at top of verify_func)
- Param ValueIds (1..param_count) are NULL in value_table — must handle in PHI check same as use-before-def check does

### Integration Points
- New error codes go in `diagnostics.h` after line 132 (after IRON_ERR_LIR_RETURN_TYPE_MISMATCH 305)
- New checks go in `verify_func()` after the existing Invariant 6 block (after line 468)
- No changes needed to `iron_lir_verify()` public API

</code_context>

<specifics>
## Specific Ideas

No specific requirements — the gaps document provides exact fix approaches. Follow the existing verifier patterns.

</specifics>

<deferred>
## Deferred Ideas

- Indirect call type checking (requires carrying function type signature on func_ptr) — tracked as AGEN-01
- Cross-function type consistency checks — future work

</deferred>

---

*Phase: 32-lir-verifier-hardening*
*Context gathered: 2026-04-02*
