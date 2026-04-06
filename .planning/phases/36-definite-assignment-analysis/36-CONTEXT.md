# Phase 36: Definite Assignment Analysis - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Implement definite assignment analysis — detect variables that may be read before being assigned on all control flow paths. This is a dataflow analysis pass that runs after type checking. Must use bounded worklist algorithm (no unbounded allocations).

</domain>

<decisions>
## Implementation Decisions

### Analysis Approach
- Standard forward dataflow analysis tracking "definitely assigned" set per variable at each program point
- Walk the AST (not CFG) — Iron's type checker works at AST level, not SSA
- Track a bitset or name set of "definitely assigned" variables
- At each use of a variable, check it's in the set on all incoming paths
- Emit `IRON_ERR_POSSIBLY_UNINITIALIZED` when a variable may be used uninitialized

### Control Flow Handling
- if/else: variable is definitely assigned after if/else only if assigned in BOTH branches
- if without else: variable is NOT definitely assigned after (else path doesn't assign)
- match with else: like if/else — assigned only if all arms assign
- loops: variable assigned inside loop body is NOT definitely assigned after (loop may execute zero times)
- early returns: if a branch returns, don't require assignment in that branch (it never reaches the use)

### Scope
- Only check `var` declarations without initializers (type annotation only)
- `val` declarations always have initializers, so skip those
- Function parameters are always initialized

### Memory Constraint
- Use bounded worklist algorithm — fixed-size bitset per scope
- No recursive path enumeration
- O(n * v) where n = statements, v = variables in scope

### Claude's Discretion
- Whether to implement as a separate pass or integrate into the type checker
- Exact data structure for tracking (bitset vs hash set)
- How to handle nested scopes (push/pop or flat)

</decisions>

<canonical_refs>
## Canonical References

### Type Checker
- `src/analyzer/typecheck.c` — Variable declarations at line ~934-970, check_stmt/check_expr

### Gaps Document
- `docs/semantic_analysis_gaps.md` §6 (Uninitialized Variables Through Partial Paths)

### Diagnostics
- `src/diagnostics/diagnostics.h` — Add new error code

</canonical_refs>

<code_context>
## Existing Code Insights

### Established Patterns
- Type checker walks AST via check_stmt/check_expr switch
- Variable declarations: `IRON_NODE_VAL_DECL` case at typecheck.c:934-970
- Scope tracking via Iron_Scope linked list

### Integration Points
- New analysis can run as a post-type-check pass on each function body
- Or integrate directly into the type checker's statement walker

</code_context>

<specifics>
## Specific Ideas

- Error message should name the variable: "variable 'x' may be used before initialization"

</specifics>

<deferred>
## Deferred Ideas

None

</deferred>

---

*Phase: 36-definite-assignment-analysis*
*Context gathered: 2026-04-03*
