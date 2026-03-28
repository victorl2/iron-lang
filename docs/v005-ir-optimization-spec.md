# v0.0.5-alpha: IR Optimization Passes — Design Specification

## Objective

Eliminate the remaining ~1.2–4.5x performance gap between Iron-generated C and hand-written C across the benchmark suite. The root cause is a single architectural issue: Iron's SSA-to-C emission produces verbose, one-instruction-per-line code that prevents clang -O3 from applying key optimizations (inlining, constant propagation, register allocation).

**Target:** 100% of benchmarks ≤ 1.2x C parity (currently 93%).

---

## Root Cause Analysis

### Current codegen pattern (example: `a + b * c`)

Iron IR (SSA):
```
_v10 = LOAD _v2       // load a
_v11 = LOAD _v4       // load b
_v12 = LOAD _v6       // load c
_v13 = MUL _v11 _v12  // b * c
_v14 = ADD _v10 _v13  // a + b * c
```

Emitted C:
```c
int64_t _v10 = _v2;
int64_t _v11 = _v4;
int64_t _v12 = _v6;
int64_t _v13 = _v11 * _v12;
int64_t _v14 = _v10 + _v13;
```

**Problem:** 5 statements, 5 variables. Clang must reconstruct the expression tree from flat SSA. With goto-based control flow and dozens of intermediates, clang's inliner gives up on functions that appear large but are semantically simple.

**Ideal C:**
```c
int64_t _v14 = _v2 + _v4 * _v6;
```

1 statement, 1 variable. Clang trivially inlines this.

### Affected benchmark patterns

| Pattern | Example Problems | Overhead |
|---------|-----------------|----------|
| Small function not inlined | median_two_sorted_arrays | 4.5x |
| 2D array `arr[i * cols + j]` expansion | count_paths_with_obstacles, min_path_sum | 1.4–1.6x |
| Recursive function call overhead | max_depth_binary_tree, word_break | 1.3–1.5x |
| Stack/histogram double-indexing `arr[stack[top]]` | largest_rect_histogram | 1.4x |
| Simple loop with redundant temporaries | target_sum, three_sum, num_islands | 1.2–1.6x |

---

## Proposed IR Optimization Passes

### Pass 1: Copy Propagation

**Goal:** Eliminate trivial copies (`_v10 = _v2`) by replacing all uses of `_v10` with `_v2`.

**When:** After phi elimination, before C emission.

**Algorithm:**
1. Scan all instructions. If `LOAD _vX` produces `_vY` and `_vX` is an alloca that was stored exactly once with `_vZ`, replace all uses of `_vY` with `_vZ`.
2. If `_vA = _vB` (identity copy via STORE+LOAD of the same value), replace uses of the loaded value with the original.
3. Iterate until no more copies can be propagated.

**Impact:** Removes 30-50% of intermediate variables. Directly helps clang's register allocator and inliner.

**Complexity:** Low. Single-pass linear scan with hashmap of replacements.

### Pass 2: Expression Inlining (Tree Reconstruction)

**Goal:** Emit compound C expressions instead of one-instruction-per-line.

**When:** During C emission (emit_c.c).

**Algorithm:**
1. Build a use-count map: for each ValueId, count how many instructions reference it.
2. If a value is used exactly once, AND the producing instruction is a pure computation (ADD, MUL, SUB, DIV, MOD, CMP, CAST, GET_INDEX on stack array), inline the expression at the use site instead of emitting a separate declaration.
3. Emit `_v14 = _v2 + _v4 * _v6;` instead of three separate statements.

**Constraints:**
- Only inline pure instructions (no side effects, no calls).
- Don't inline across basic block boundaries.
- Don't inline if the result is used in a phi node or stored to memory.
- Limit inline depth to prevent unreadable C (max 4 levels).

**Impact:** Reduces emitted C size by 40-60%. Enables clang to inline small functions that currently appear too large. Primary fix for median_two_sorted_arrays.

**Complexity:** Medium. Requires use-count analysis and recursive expression building during emission.

### Pass 3: Dead Code Elimination

**Goal:** Remove instructions whose results are never used.

**When:** After copy propagation, before C emission.

**Algorithm:**
1. Mark all instructions that have side effects (CALL, STORE, RETURN, SET_INDEX, SET_FIELD, PRINT, FREE) as live.
2. Mark all instructions referenced by live instructions as live (transitive closure).
3. Remove all non-live instructions.

**Impact:** Eliminates dead LOAD/ADD/MUL chains left after copy propagation. Reduces function size further, improving inlining.

**Complexity:** Low. Standard reverse-reachability on the def-use graph.

### Pass 4: Constant Folding

**Goal:** Evaluate compile-time constant expressions in the IR.

**When:** After copy propagation, before C emission.

**Algorithm:**
1. If all operands of an arithmetic instruction are CONST_INT, compute the result and replace with a new CONST_INT.
2. Propagate through STORE/LOAD chains where the stored value is constant.
3. Fold comparisons with known outcomes (enables dead branch elimination).

**Impact:** Small for most benchmarks (clang already folds constants). Helps in cases where Iron's separate-TU compilation model prevents clang from seeing constants.

**Complexity:** Low. Single-pass with constant lattice.

### Pass 5: Strength Reduction for Index Expressions

**Goal:** Optimize `i * cols + j` array index patterns by hoisting loop-invariant multiplications.

**When:** During or after copy propagation.

**Algorithm:**
1. Detect the pattern `MUL(loop_var, loop_invariant)` inside a loop body.
2. Replace with an induction variable: `row_base += cols` at each iteration instead of `i * cols`.
3. For 2D DP problems, this eliminates one multiply per inner-loop iteration.

**Impact:** Directly fixes count_paths_with_obstacles, min_path_sum (currently 1.4–1.6x).

**Complexity:** Medium. Requires loop detection (already have for-loop structure in IR) and induction variable insertion.

### Pass 6: Redundant STORE/LOAD Elimination

**Goal:** Remove STORE+LOAD pairs where no intervening instruction can modify the stored value.

**When:** After copy propagation.

**Algorithm:**
1. Track the last value stored to each alloca.
2. When a LOAD is encountered for that alloca, if no intervening instruction could have modified it (no CALL with aliasing potential, no other STORE to the same alloca), replace the LOAD with the stored value directly.
3. This is a simplified form of mem2reg (which we already do via phi elimination, but the current phi elimination leaves some redundant patterns).

**Impact:** Eliminates the "reload after every SET_INDEX" pattern where the compiler stores the array pointer back and immediately reloads it. Fixes the `_v6 = _v8; ... _v6 = _v26;` pointer re-aliasing issue.

**Complexity:** Medium. Requires alias analysis (conservative: any CALL invalidates all allocas).

---

## Implementation Plan

### Phase 1: Copy Propagation + Dead Code Elimination
- **Files:** New `src/ir/ir_optimize.c` + `src/ir/ir_optimize.h`
- **Entry point:** Called from `iron_ir_emit_c()` after `phi_eliminate()`, before emission.
- **Expected impact:** 15-25% reduction in emitted C size. Fixes ~3 borderline benchmarks.
- **Effort:** Small.

### Phase 2: Expression Inlining
- **Files:** Modify `src/ir/emit_c.c` emission logic.
- **Approach:** Build use-count map during pre-scan, then during instruction emission, recursively inline single-use pure expressions.
- **Expected impact:** Fixes median_two_sorted_arrays (4.5x → ~1.2x) and most 1.3-1.6x near-miss problems.
- **Effort:** Medium.

### Phase 3: Strength Reduction + Redundant STORE/LOAD Elimination
- **Files:** Extend `src/ir/ir_optimize.c`.
- **Expected impact:** Fixes 2D DP problems and recursive patterns.
- **Effort:** Medium.

### Phase 4: Constant Folding
- **Files:** Extend `src/ir/ir_optimize.c`.
- **Expected impact:** Minor. Insurance against edge cases.
- **Effort:** Small.

---

## Verification

Each pass must:
1. Pass all 127 existing benchmarks (correctness).
2. Pass all 42 integration tests.
3. Pass all unit tests (IR verify, emit, lower).
4. Show measurable improvement on the target benchmarks (run before/after comparison).

Benchmark targets for v0.0.5-alpha:
- median_two_sorted_arrays: ≤ 1.2x (currently 4.5x)
- count_paths_with_obstacles: ≤ 1.2x (currently 1.4x)
- min_path_sum: ≤ 1.2x (currently 1.6x)
- largest_rect_histogram: ≤ 1.2x (currently 1.4x)
- max_depth_binary_tree: ≤ 1.2x (currently 1.5x)
- target_sum: ≤ 1.2x (currently 1.6x)
- All 127 benchmarks passing at their configured threshold.

---

## Non-Goals for v0.0.5

- LLVM backend (future milestone)
- Register allocation (clang handles this)
- Vectorization (clang handles this)
- Interprocedural optimization beyond parameter modes (complex, diminishing returns)
- Garbage collection or advanced memory management
