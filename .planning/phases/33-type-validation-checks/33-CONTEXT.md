# Phase 33: Type Validation Checks - Context

**Gathered:** 2026-04-02
**Status:** Ready for planning

<domain>
## Phase Boundary

Add four type-checker-level validation checks to `typecheck.c` (and `hir_lower.c` for compound overflow): match exhaustiveness, cast safety, string interpolation stringability, and compound assignment overflow detection. These catch invalid code that currently silently passes through to the C backend.

</domain>

<decisions>
## Implementation Decisions

### Match Exhaustiveness
- Check that match on enum types covers all variants or has an else clause
- For non-enum types (Int, String, etc.): **require** else clause — error if missing
- Check associated data patterns at full depth (not just variant names) — e.g., `Color.RGB(0,0,0)` vs `Color.RGB(_,_,_)` are distinct patterns
- Duplicate match arms (same variant covered twice) produce a **compile error** — `IRON_ERR_DUPLICATE_MATCH_ARM`
- Emit `IRON_ERR_NONEXHAUSTIVE_MATCH` listing uncovered variants
- Data needed: `Iron_EnumDecl` in `ast.h:131-137` has `variants` array with `variant_count`, resolved enum type in `types.h:72-75` holds pointer to decl

### Cast Safety
- Validate source expression type is numeric or bool before allowing primitive cast
- Emit `IRON_ERR_INVALID_CAST` when source type is non-numeric, non-bool (e.g., String to Int, MyObject to Int)
- **Narrowing warnings:** only fire when data loss is likely — wider-to-narrower where the source type's range exceeds the target's (Int64→Int8 warns, but not when value is provably in range)
- Emit `IRON_WARN_NARROWING_CAST` for risky narrowing
- **Bool casting:** Bool→Int is allowed (gives 0/1), but Int→Bool is **not allowed** (must use explicit comparison like `x != 0`)
- **Literal range:** `Int8(300)` is a compile-time **error** (not warning) — emit `IRON_ERR_CAST_OVERFLOW` when constant value provably doesn't fit target type

### String Interpolation
- Implicitly stringifiable types: primitives (Int, Float), String, Bool, enums (variant names), and any type with a `to_string()` method
- Non-stringifiable types **fall back to address printing** (like C's `%p`) — emit `IRON_WARN_NOT_STRINGABLE` as a warning (not error) so it compiles but user gets a heads-up
- Check each interpolation part's resolved type against the stringifiable set

### Compound Overflow
- Detect compound assignments (`+=`, `-=`, `*=`, `/=`) on narrow integer types (Int8, Int16, UInt8, etc.)
- Emit `IRON_WARN_POSSIBLE_OVERFLOW` when target type is narrower than platform int and RHS is not a compile-time constant that fits
- When RHS is a constant and provably fits: no warning
- Check happens in `typecheck.c` at the assignment level (not in `hir_lower.c` where desugaring happens)

### Claude's Discretion
- Helper function organization for match exhaustiveness (whether to extract variant collection into a helper)
- Exact narrowing threshold logic (which type pairs trigger vs don't)
- How to detect `to_string()` method presence on user types

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Type Checker
- `src/analyzer/typecheck.c` — Main type checker; match at line ~1237, cast at line ~451, interp_string at line ~305
- `src/analyzer/types.h` — Type definitions, `iron_type_equals()`, enum type with decl pointer
- `src/analyzer/types.c` — Type comparison implementations

### AST
- `src/parser/ast.h` — `Iron_EnumDecl` (line 131-137), `Iron_MatchStmt` (line 277), variant_count, match cases
- `src/parser/ast.h` — `Iron_CastExpr` or equivalent call expression with `is_primitive_cast`

### HIR Lowering
- `src/hir/hir_lower.c` — Compound assignment desugaring at line ~262-278, `is_compound_assign()` at line 273

### Diagnostics
- `src/diagnostics/diagnostics.h` — Existing error codes; add new ones after 307

### Gaps Document
- `docs/semantic_analysis_gaps.md` §1 (Match Exhaustiveness), §5 (Cast Safety), §11 (String Interpolation), §10 (Compound Overflow)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `iron_type_equals()`: Type comparison, already used in verify.c (Phase 32)
- `Iron_EnumDecl->variants` + `variant_count`: Variant list available on enum types
- `iron_diag_emit()`: Standard diagnostic emission pattern
- `is_numeric_or_bool` check in typecheck.c line ~456: Already partially implemented for cast target — extend to validate source too

### Established Patterns
- Type checker switches on `node->kind` in `check_expr` and `check_stmt`
- Resolved types stored on `node->resolved_type`
- Errors use `snprintf` into stack buffer with `iron_diag_emit`
- Error codes are sequential `#define`s in `diagnostics.h`

### Integration Points
- Match exhaustiveness: extends existing `IRON_NODE_MATCH` case in `check_stmt` (typecheck.c:1237)
- Cast safety: extends existing `is_numeric_or_bool` block in `check_expr` (typecheck.c:451-486)
- String interpolation: extends existing `IRON_NODE_INTERP_STRING` case in `check_expr` (typecheck.c:305-312)
- Compound overflow: new check in `check_stmt` at assignment handling, or in `check_expr` for compound assign operators

</code_context>

<specifics>
## Specific Ideas

- Match exhaustiveness should list ALL uncovered variants in the error message, not just "incomplete"
- Duplicate match arm error is a separate error code from non-exhaustive
- The stringability fallback to address printing is a warning, not error — user-friendly for debugging
- Literal range errors for casts should show the value and the valid range (e.g., "300 does not fit in Int8 (range: -128..127)")

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 33-type-validation-checks*
*Context gathered: 2026-04-02*
