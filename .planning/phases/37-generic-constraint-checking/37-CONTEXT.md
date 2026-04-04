# Phase 37: Generic Constraint Checking - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Add generic constraint validation at instantiation sites. When a generic function or type is used with concrete type arguments, verify the concrete type satisfies declared constraints (e.g., `T: Comparable`).

</domain>

<decisions>
## Implementation Decisions

### Constraint Validation
- At each generic instantiation site (call to generic function, construction of generic type), substitute concrete type arguments into constraints
- Check that concrete type satisfies the constraint — either implements the required interface or has required methods
- Emit `IRON_ERR_CONSTRAINT_NOT_SATISFIED` naming the constraint and the failing type

### What Counts as Satisfying a Constraint
- If constraint is an interface type: check that the concrete type implements that interface (existing interface conformance check)
- If constraint is a trait-like type: check the concrete type has all required methods
- For simpler cases: check that required methods/operations exist on the concrete type

### Integration Points
- Generic params stored in `ast.h:119-120` (`generic_params`, `generic_arg_count`)
- Constraint field in `types.h:100-104` (`generic_param.constraint`, NULL if unconstrained)
- Type checker needs new logic at function call and type construction sites

### Claude's Discretion
- Where exactly to hook the validation (in check_expr at CALL, or in a separate pass)
- How to check "type implements interface" — may leverage existing interface conformance logic
- How to handle nested generic constraints

</decisions>

<canonical_refs>
## Canonical References

### Type System
- `src/analyzer/types.h` — Generic param with constraint at line 100-104
- `src/analyzer/types.c` — Type constructors including `iron_type_make_generic_param()`
- `src/analyzer/typecheck.c` — Type checker, function calls, type construction

### AST
- `src/parser/ast.h` — `generic_params` and `generic_arg_count` on function/type declarations (lines 119-120, 150-151, 165-166)

### Diagnostics
- `src/diagnostics/diagnostics.h` — Add new error code

### Gaps Document
- `docs/semantic_analysis_gaps.md` §4 (Generic Type Constraints)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- Interface conformance checking already exists in the type checker
- `iron_type_equals()` for type comparison
- Generic param constraint field is already stored but never checked

### Integration Points
- Hook into function call resolution in typecheck.c
- Hook into type construction in typecheck.c

</code_context>

<deferred>
## Deferred Ideas

- Higher-kinded type constraint checking — tracked as AGEN-02
- Indirect calls with generic signatures — tracked as AGEN-01

</deferred>

---

*Phase: 37-generic-constraint-checking*
*Context gathered: 2026-04-03*
