# Phase 34: Bounds Checking - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Add compile-time bounds checking for array indices and slice bounds in `typecheck.c`. When index/bounds are compile-time constants and array size is known, validate they're in range.

</domain>

<decisions>
## Implementation Decisions

### Array Bounds
- Validate constant array indices against known array sizes: `0 <= index < size`
- Emit `IRON_ERR_INDEX_OUT_OF_BOUNDS` for provably out-of-bounds constant indices (e.g., `arr[5]` on size-3 array, `arr[-1]`)
- Validate that index expressions resolve to integer types — emit type error if not
- Array type carries size info in `types.h:94-98` (`array.size`, -1 = dynamic)
- Only check when both index is a constant literal AND array has a known size (size >= 0)
- Non-constant indices or dynamic arrays: skip (runtime check territory)

### Slice Bounds
- Validate that slice start and end expressions resolve to integer types
- When both start and end are compile-time constants: validate `0 <= start <= end`
- When all three (start, end, array size) are constants: validate `end <= size`
- Emit `IRON_ERR_INVALID_SLICE_BOUNDS` for invalid constant slice bounds
- Non-constant bounds: skip

### Claude's Discretion
- Helper function for extracting constant integer value from AST nodes
- Whether to combine array/slice checks into one plan or split them

</decisions>

<canonical_refs>
## Canonical References

### Type Checker
- `src/analyzer/typecheck.c` — Array index at line ~796, slice at line ~809
- `src/analyzer/types.h` — Array type with `array.elem` and `array.size` fields (line 94-98)

### Diagnostics
- `src/diagnostics/diagnostics.h` — Add new error codes after existing ones

### Gaps Document
- `docs/semantic_analysis_gaps.md` §7 (Array Bounds) and §8 (Slice Bounds)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `iron_type_is_integer()`: Already exists for type classification
- `value_fits_type()` and `type_bit_width()`: Added in Phase 33, may be useful for range checks
- Integer literal value stored as `Iron_IntLit->value` (string, use `strtoll`)

### Established Patterns
- Type checker switches on node->kind in check_expr
- Array index: `IRON_NODE_INDEX` case, slice: `IRON_NODE_SLICE` case
- Error emission via `iron_diag_emit`

### Integration Points
- Array bounds: extend `IRON_NODE_INDEX` case in `check_expr` (typecheck.c:796-807)
- Slice bounds: extend `IRON_NODE_SLICE` case in `check_expr` (typecheck.c:809-817)

</code_context>

<specifics>
## Specific Ideas

No specific requirements — follow the patterns from Phase 33.

</specifics>

<deferred>
## Deferred Ideas

- Runtime bounds check insertion in generated C — tracked as v2 requirement RTSAFE-01

</deferred>

---

*Phase: 34-bounds-checking*
*Context gathered: 2026-04-03*
