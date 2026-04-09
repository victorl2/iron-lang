# Phase 54: Test Hardening - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Systematically test the compiler's behavior under edge cases, stress conditions, and optimization compositions. Also fix benchmark flakiness. Does NOT add new features or change optimization behavior — purely testing and benchmark threshold adjustments.

</domain>

<decisions>
## Implementation Decisions

### Edge case coverage
- **All four edge cases are high priority**: empty collections, all-filtered-out, single element, single implementor
- **Each edge case tested through all optimization paths**: fusion, SoA, dead field, monomorphic, arena, compression
- **Verification depth: runtime output + generated C checks** — tests verify correct output AND grep the generated C for expected patterns (e.g., no SplitList for single-implementor, uint8_t for compressed fields)

### Stress test boundaries
- **10K elements** — exercises arena growth, fusion over large arrays. Compiles in seconds.
- **10 implementors** — tests tag dispatch scaling, split collection with 10 sub-arrays, dead field analysis across 10 types
- **30 second timeout** — if a stress test takes >30s to compile+run, it fails. Prevents CI hangs from accidental O(n^2) behavior.

### Composition test matrix
- **4 focused composition tests**:
  - SoA + fusion (fused loop accessing per-field arrays)
  - Dead field + compression (reduced storage struct with narrowed types)
  - Monomorphic + fusion (flat array fusion, no split overhead)
  - Arena + SoA + dead field (triple composition)
- **Plus 1 mega test** combining ALL optimizations: SoA + fusion + dead field + compression + monomorphic + arena + annotations
- Both focused tests AND mega test (not either/or)

### Test organization
- **All in tests/integration/** — prefixed names: `edge_*`, `stress_*`, `compose_*`
- Uses existing test runner, no new CMake targets needed

### Benchmark robustness
- **Raise speed threshold from 1.5x to 2.5x** — tolerates CI runner variance while still catching real regressions (3x+ would indicate a real problem)
- **Raise memory threshold from 1.5x to 2.0x** — arena overhead means Iron uses slightly more memory; 2.0x gives breathing room
- Threshold changes apply in the benchmark runner script, not in test infrastructure

### Claude's Discretion
- Exact Iron source code for each test (struct definitions, field counts, method bodies)
- How to generate 10K elements in Iron (loop, literal, or helper function)
- Which generated C patterns to grep for in verification
- How to structure the mega test to exercise all optimizations without being fragile

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Existing test infrastructure
- `tests/integration/` — All existing integration tests (274 files). New tests go here with prefixed names.
- `tests/benchmark/run_benchmarks.sh` — Benchmark runner with speed/memory thresholds. Threshold values to update.
- `CMakeLists.txt` — Test registration via `add_test()`. New .iron/.expected files auto-discovered.

### Optimization code to test
- `src/lir/emit_c.c` — Core orchestration, monomorphic detection, pre-scan passes
- `src/lir/emit_split.c` — Split collection emission (SplitList structs, push, free)
- `src/lir/emit_fusion.c` — Fused loop emission
- `src/lir/emit_structs.c` — Struct generation (Stor types, tagged unions)
- `src/lir/value_range.c` — Value range analysis, conditional narrowing
- `src/lir/layout_analysis.c` — SoA/AoS selection, dead field detection

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Integration test pattern**: `.iron` source + `.expected` output file. Test runner compiles with `ironc run`, diffs stdout against expected.
- **Benchmark runner**: `tests/benchmark/run_benchmarks.sh` compares Iron vs C implementations. Threshold constants are at the top of the script.
- **Existing edge-case-adjacent tests**: `mono_multi_type_no_collapse.iron` (multi-type), `fusion_chain_break.iron` (partial fusion), `fusion_intermediate_escape.iron` (escape analysis)

### Established Patterns
- Tests are self-contained `.iron` files that print results to stdout
- Expected files contain exact stdout output (newline-terminated)
- No test framework inside Iron — just `println()` assertions

### Integration Points
- New test files auto-discovered by the test runner (glob `tests/integration/*.iron`)
- Benchmark threshold changes in `run_benchmarks.sh` — single line edits

</code_context>

<specifics>
## Specific Ideas

- The mega test should combine: an interface with 3+ implementors, SoA layout annotation on the collection, a fused map+filter+reduce chain, dead fields on at least one implementor, compressed small-range fields, and arena allocation (>8 elements to trigger growth)
- Stress test with 10 implementors should define 10 simple objects (Shape1..Shape10) each implementing an interface with area() method
- Empty collection test should verify that fused chains on empty arrays don't crash and return the correct identity values (0 for sum, init for reduce)
- Generated C pattern checks: grep for `uint8_t` (compression), absence of `Iron_SplitList_` (monomorphic), presence of `_iron_sl_realloc_tracked` (arena)

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 54-test-hardening*
*Context gathered: 2026-04-09*
