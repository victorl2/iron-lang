# Phase 38: Concurrency Safety - Context

**Gathered:** 2026-04-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Extend concurrency checking in `concurrency.c` to cover field access and array index mutations in parallel/spawn blocks (not just bare identifiers), add capture analysis for spawn blocks, and detect read-write races.

</domain>

<decisions>
## Implementation Decisions

### Field/Array Mutation Detection
- Currently `check_stmt_for_mutation` only checks `as->target->kind == IRON_NODE_IDENT` for bare identifiers
- Extend to also check `IRON_NODE_FIELD_ACCESS` and `IRON_NODE_INDEX` targets
- Extract the base variable name recursively (reuse pattern from escape.c's extended `expr_ident_name`)
- If base name is an outer-scope variable, flag the mutation

### Spawn Capture Analysis
- Walk spawn block bodies to identify all outer-scope variable references
- Track which are reads vs writes
- Flag mutable captures as potential data races

### Read-Write Race Detection
- When a spawn block reads a variable that the outer scope (or another spawn) writes, emit a diagnostic
- Conservative: any shared mutable access across spawn boundaries is flagged
- Don't flag partitioned parallel-for patterns where each iteration accesses different indices

### Error/Warning Codes
- Reuse existing concurrency warning codes where possible (E0208 for outer mutation)
- Add new warning for read-write races and spawn capture issues

### Claude's Discretion
- Whether to add a separate spawn analysis function or extend the existing parallel-for checker
- How deep to analyze spawn captures (just identifiers or through field chains)
- Whether partitioned parallel-for (arr[i] where i is the loop var) should be exempted

</decisions>

<canonical_refs>
## Canonical References

### Concurrency Checker
- `src/analyzer/concurrency.c` — Current checker (253 lines), handles parallel-for mutation on bare identifiers

### Gaps Document
- `docs/semantic_analysis_gaps.md` §12 (Concurrency Beyond Parallel-For)

### Escape Analysis (pattern reference)
- `src/analyzer/escape.c` — Extended `expr_ident_name()` from Phase 35 that handles field/index recursion

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `name_is_local()` in concurrency.c: checks if a variable name is local to the parallel block
- `expr_ident_name()` pattern from escape.c: recursive field/index name extraction
- `check_stmt_for_mutation()`: existing mutation detection function

### Integration Points
- Extend `check_stmt_for_mutation()` to handle field/index targets
- Add spawn block handling (currently only parallel-for is checked)
- Add read-write race detection for spawn blocks

</code_context>

<deferred>
## Deferred Ideas

- Full deadlock detection — tracked as ACONC-01
- Task lifetime analysis — tracked as ACONC-02
- Effect-based race detection — tracked as ACONC-03

</deferred>

---

*Phase: 38-concurrency-safety*
*Context gathered: 2026-04-03*
