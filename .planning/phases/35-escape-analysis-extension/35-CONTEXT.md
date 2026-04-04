# Phase 35: Escape Analysis Extension - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Extend escape analysis in `escape.c` to track heap values through field assignments, array index assignments, and function argument passing. Currently only bare identifier assignments are tracked.

</domain>

<decisions>
## Implementation Decisions

### Field Escape
- `obj.field = heap_val` must mark heap_val as escaped
- Extend `expr_ident_name()` or assignment tracking to recognize field-access targets (IRON_NODE_FIELD_ACCESS)
- When RHS of any assignment is a known heap binding, mark escaped regardless of LHS form

### Array Index Escape
- `arr[i] = heap_val` must mark heap_val as escaped
- Extend assignment tracking to recognize index-access targets (IRON_NODE_INDEX)

### Function Argument Escape
- When a heap binding is passed as a function argument, mark it as escaped (conservative but safe)
- Check each argument in call expressions against the heap binding set

### Implementation Approach
- The key function to extend is in escape.c where assignment tracking uses `expr_ident_name()`
- Either extend `expr_ident_name()` to return names from field/index targets, or add parallel checks in the assignment handler
- Keep the HeapBinding struct as-is — just expand what triggers "escaped" marking

### Claude's Discretion
- Whether to extend `expr_ident_name()` or add separate checks
- How deep to recurse for chained field access (e.g., `a.b.c = heap_val`)

</decisions>

<canonical_refs>
## Canonical References

### Escape Analysis
- `src/analyzer/escape.c` — Current escape analysis; HeapBinding struct, expr_ident_name(), assignment tracking at line ~138-152

### Gaps Document
- `docs/semantic_analysis_gaps.md` §9 (Escape Through Fields and Arrays)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `expr_ident_name()`: Currently only recognizes IRON_NODE_IDENT — extend or supplement
- `HeapBinding` struct: name + heap_node pair
- Assignment tracking loop in escape.c

### Integration Points
- Extend the assignment handler in escape.c (line ~138-152) for field/index LHS
- Add function argument check in the call expression handler

</code_context>

<specifics>
## Specific Ideas

No specific requirements — follow the fix approach from the gaps document.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 35-escape-analysis-extension*
*Context gathered: 2026-04-03*
