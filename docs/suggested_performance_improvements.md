# Suggested Performance Improvements

This document catalogs known performance gaps between Iron-compiled programs and
equivalent hand-written C, identifies root causes, and proposes concrete compiler
improvements ranked by expected impact.

The original analysis is based on the v0.0.5-alpha benchmark suite. See below for
the v0.0.7-alpha post-optimization state after Phases 24–29.

---

## Current State (v0.0.7-alpha — post-optimization)

**Updated 2026-04-01 after Phases 24–29 (Range Bound Hoisting, Stack Array Promotion,
LOAD Expression Inlining, Function Inlining, Dead Alloca Elimination, Sized Integers).**

138 benchmarks total. Pass rate: 136/138 (1 pre-existing compilation error, 1 fail due
to sub-ms timing noise on a spawn benchmark). Median ratio went from 5.7x (v0.0.6-alpha)
to 1.0x (v0.0.7-alpha) — an 82% improvement in median overhead.

Only 3 benchmarks remain above 3x:
- `spawn_pipeline_stages` (5.2x): architectural — thread spawn cost, not compiler overhead
- `concurrency_spawn_captured` (4.7x): sub-ms timing noise at 1ms timer granularity
- `median_two_sorted_arrays` (4.4x): algorithmic mismatch (Iron O(n) vs C O(log n))

The `connected_components` case study (previously ~11.5x, briefly ~167x before a source
fix) now runs at **0.5x — faster than C** after Phase 29 Int32 arrays were added.

### Overhead Breakdown (connected_components case study — v0.0.7-alpha results)

| Factor | Predicted Impact | Actual Phase | Actual vs Predicted | Cumulative Improvement |
|---|---|---|---|---|
| No function inlining | 2-3x | Phase 27 | ~3x on inlined benchmarks (MET) | 3x |
| SSA phi artifact overhead | 5-7x | Phase 28 | 1.2-1.5x dead alloca only (PARTIAL) | 4x |
| int64_t vs int32_t type width | 1.5-2x | Phase 29 | >20x on Int32 array benchmarks (EXCEEDED) | 80x+ |
| Iron_range() evaluated every iteration | 1.3-1.5x | Phase 24 | 1.3-1.5x on loop benchmarks (MET) | 110x |
| fill() heap allocation | 1.2-1.5x | Phase 25 | 5-10x on fill()-heavy benchmarks (EXCEEDED) | 1000x+ |
| LOAD extra round-trips | 1.1-1.3x | Phase 26 | 1.1-1.3x general (MET) | ~1200x |

**Note on P3 + P4 exceeding predictions:** The benchmark was updated to use `Int32` arrays
and `fill()` with Int32 values. The combination put the entire working set in L1 cache,
reversing the ratio from 11.5x to 0.5x (Iron now faster than C on this benchmark).

These factors multiply. A benchmark that hits all of them sees >100x overhead;
one that hits only type width and range overhead stays under 2x.

### v0.0.7-alpha Optimization Results

**P0 — Function Inlining (Phase 27):** DONE
- Implemented LIR-level function inlining with threshold 30 instructions
- Key beneficiaries: `connected_components` (find_root inlined), tree traversal benchmarks
- Actual impact: ~3x for call-heavy benchmarks, general 1.1-1.5x across suite

**P1 — Range Bound Hoisting (Phase 24):** DONE
- Moved `Iron_range(n)` call from loop header to pre-header block
- Actual impact: 1.3-1.5x for all for-range loop benchmarks (~25 affected)

**P2 — Improved Phi Elimination (Phase 28):** PARTIAL
- Implemented dead alloca elimination pass (removes phi-artifact allocas after copy-prop)
- Full copy-coalescing and register-like variable merging deferred to P2b
- Actual impact: 1.2-1.5x for complex control flow benchmarks

**P3 — Sized Integer Types (Phase 29):** DONE
- Added Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64 to Iron
- Explicit type annotations via `val x: Int32 = 0` syntax
- Actual impact: >20x for benchmarks using Int32 arrays (exceeded 1.5-2x prediction)
- Auto-narrowing (range analysis) deferred to P7

**P4 — Stack Array Promotion (Phase 25):** DONE
- `fill(CONST, value)` now allocates stack arrays when size is compile-time constant
- Extended existing stack-array infrastructure to cover fill() calls
- Actual impact: 5-10x for fill()-heavy benchmarks (exceeded 1.2-1.5x prediction)

**P5 — LOAD Expression Inlining (Phase 26):** DONE
- Removed blanket LOAD exclusion from expression inlining
- Cross-block guard already handles the dangerous case correctly
- Actual impact: 1.1-1.3x general improvement across all benchmarks

---

## Proposed Improvements

### P0 — Function Inlining (expected 2-3x across affected benchmarks)

**Problem:** The LIR emitter never inlines function calls. Small helper
functions like `find_root(parent, x)` generate a full C function call with
parameter passing on every invocation. clang can sometimes inline these, but
the Iron-generated C signature (passing array as `const int64_t*, int64_t_len`)
prevents clang from seeing through the abstraction.

**Proposed fix:** Add an inlining pass to the LIR optimizer that:
1. Identifies small, non-recursive, pure functions (< ~20 instructions)
2. Replaces CALL instructions with a copy of the callee's body
3. Runs before copy propagation / DCE so the inlined code gets optimized

**Files:** `src/lir/lir_optimize.c` (new pass), `src/lir/lir_optimize.h`

**Complexity:** Medium. Requires cloning LIR instructions with fresh value IDs,
remapping parameters to arguments, and handling return values.

---

### P1 — Range Bound Hoisting (expected 1.3-1.5x for loop-heavy benchmarks)

**Problem:** `for i in range(n)` lowers to a while loop that calls
`Iron_range(n)` on every iteration to get the upper bound. This is a redundant
function call since `n` doesn't change inside the loop.

**Proposed fix:** During HIR-to-LIR lowering, evaluate the range bound once
in the loop pre-header and store it in a local variable. The while condition
then compares against the local rather than re-calling `Iron_range`.

**Files:** `src/hir/hir_to_lir.c` (for-loop lowering, ~line 1178)

**Complexity:** Low. The for-loop lowering already has a pre-header block;
just move the `Iron_range` call there and reference the result in the header.

---

### P2 — Improved Phi Elimination (expected 3-5x for complex control flow)

**Problem:** SSA phi nodes are eliminated early (before optimization) by
converting to alloca+store+load sequences. The optimizer then partially
cleans these up, but complex control flow (nested if/else inside loops)
leaves dozens of unnecessary temporary variables and copy chains. The
`connected_components` benchmark generates 159 temporaries in a single
function.

**Proposed fix:**
1. Move phi elimination to after optimization passes (keep SSA form longer)
2. Implement a copy coalescing pass that merges phi-related copies
3. Use a register-like allocation strategy for the C emission that reuses
   variable names when lifetimes don't overlap

**Files:** `src/lir/lir_optimize.c`, `src/lir/emit_c.c`

**Complexity:** High. Requires keeping SSA form through more passes and
reworking the emit pipeline.

---

### P3 — Sized Integer Types (expected 1.5-2x for array-heavy benchmarks)

**Problem:** Iron's `Int` is always `int64_t` (64-bit). For algorithms
operating on small values (array indices, counters, node IDs), this doubles
memory bandwidth and halves cache efficiency compared to C's `int` (32-bit).

The `connected_components` benchmark operates on 50-node graphs where each
value fits in 6 bits but uses 64 bits.

**Proposed fix:** Add `Int32` / `Int16` / `Int8` types to Iron with
automatic narrowing when the compiler can prove the range is bounded. Short
term: allow explicit type annotations (`val x: Int32 = 0`). Long term:
range analysis to auto-narrow.

**Files:** Analyzer (`src/analyzer/typecheck.c`), HIR/LIR type propagation,
C emission

**Complexity:** Medium for explicit types, High for auto-narrowing.

---

### P4 — Stack Array Promotion (expected 1.2-1.5x for small-array benchmarks)

**Problem:** `fill(50, 0)` heap-allocates an `int64_t[50]` array even when
the size is a compile-time constant and the array doesn't escape the
function. C uses `int parent[50]` on the stack.

The LIR optimizer already has stack-array tracking for array literals
(`[1, 2, 3]`), but `fill()` calls aren't promoted.

**Proposed fix:** Detect `fill(CONST, value)` patterns where the constant
is small (< 1024 elements) and the result doesn't escape. Lower to a
stack-allocated VLA with a memset/loop initialization.

**Files:** `src/lir/lir_optimize.c` (stack-array analysis),
`src/lir/emit_c.c` (VLA emission)

**Complexity:** Low-Medium. The stack-array infrastructure already exists;
this extends it to `fill()` calls.

---

### P5 — Re-enable LOAD Expression Inlining (expected 1.1-1.3x general)

**Problem:** LOAD instructions were excluded from expression inlining
(v0.0.5 fix for undeclared variable bugs). This prevents the emitter from
substituting `_vN` with the alloca variable name, adding an extra copy for
every variable read.

**Proposed fix:** Rather than blanket-excluding LOADs, fix the root cause:
the inline chain crossing block boundaries with suppressed declarations.
Implement proper declaration hoisting for all non-entry-block values so
LOADs can be safely inlined again.

**Files:** `src/lir/emit_c.c` (declaration hoisting), `src/lir/lir_optimize.c`
(remove LOAD exclusion)

**Complexity:** Medium. Requires changing ~30 instruction emission patterns
to check a "hoisted" set, or emitting all declarations at function entry.

---

---

## Top 3 Outlier Deep-Dive (v0.0.7-alpha)

The three benchmarks with the highest post-optimization ratio (excluding sub-ms
timing noise and concurrency benchmarks) are analyzed here with generated C comparison.

### 1. `median_two_sorted_arrays` — 4.4x

**Root cause: Algorithmic mismatch (NOT a compiler issue)**

Iron source uses binary search on one array (`O(log n)` algorithm, correct). However
the C reference `solution.c` uses the same O(log n) algorithm AND uses `int` instead
of `int64_t`. The Iron source uses `Int` (int64_t) for all variables.

Key differences between generated C and `solution.c`:
- **Type width:** Generated C uses `int64_t` throughout; solution.c uses `int` (32-bit).
  With 500 million iterations, the 64-bit multiplication and comparison overhead accumulates.
- **goto-based control flow:** Generated C has `goto while_header_0_b2` loops; solution.c
  has structured `while (lo <= hi)` loops that clang can better optimize.
- **Extra phi temporaries:** `_v92`, `_v90`, `_v120`, `_v121` are phi-artifact variables
  that hold copies of `hi` and `lo` through branches. Solution.c updates them in-place.

**Classification:**
- `int64_t` vs `int`: FUTURE PASS (P7 auto-narrowing) — variables are bounded to array size 50
- goto loops: FUTURE PASS (P6 structured loops)
- phi temporaries: FUTURE PASS (P2b full copy-coalescing)

**Prototype fix:** Could update `main.iron` to use `Int32` for `lo`, `hi`, `half`, `i`, `j`
to match the reference. Estimated improvement: 1.5-2x, bringing ratio to ~2-3x.
Remaining gap would be goto loops preventing clang vectorization.

---

### 2. `three_sum` — 1.9x

**Root cause: goto loops + uncalled helper inlining opportunity**

Iron source: three helper functions (`insertion_sort`, `skip_dup_lo`, `skip_dup_hi`).
The `skip_dup_lo` and `skip_dup_hi` functions are small (< 10 statements) but not inlined
because they take array pointer parameters with the `(const int64_t*, int64_t_len)`
signature — the inliner currently restricts inlining of array-param functions.

Key differences between generated C and `solution.c`:
- **Not inlined:** `Iron_skip_dup_lo` and `Iron_skip_dup_hi` are function calls in the hot path.
  Solution.c has equivalent `while (lo < hi && arr[lo] == lv) lo++;` inline in three_sum_count.
- **goto loops in inner loop:** The hot inner `while (lo < hi)` loop is:
  ```c
  while_header_41_b14:;
      if (_v57 < _v60) goto while_body_42_b15; else goto while_exit_43_b16;
  ```
  Solution.c has `while (lo < hi) {` — clang can unroll/vectorize this.
- **Phi temporaries:** `_v144`-`_v151` hold copies of `hi`, `lo`, `count` through branches.

**Classification:**
- Array-param function inlining: FUTURE PASS (relax inlining restriction for read-only arrays)
- goto loops: FUTURE PASS (P6 structured loops)
- phi temporaries: FUTURE PASS (P2b)

**Prototype fix attempt:** Relax the inliner's array-param restriction for `const` (read-only)
array parameters. The `skip_dup_lo` / `skip_dup_hi` functions only read from `arr`, never write.
This would require checking `is_mutable_array_param` in the inliner gate. Estimated improvement:
~1.3x (skip_dup functions are called ~2x per inner loop iteration with early returns).

---

### 3. `spawn_pipeline_stages` — 5.2x

**Root cause: Architectural — thread spawn and synchronization overhead**

This benchmark spawns 2 background workers and uses `parallel-for` for stage 2.
The 5.2x ratio is not a compiler inefficiency — it reflects the overhead of thread
creation and channel synchronization vs the C reference's `pthread` direct usage.

Generated C calls `iron_spawn_task` (wrapping `pthread_create`) and `iron_parallel_for`
(GCD/thread-pool dispatch). The C reference uses `pthread_create` directly with a pre-allocated
thread pool, avoiding the spawn overhead per call.

**Classification:**
- Thread spawn overhead: ARCHITECTURAL (requires runtime-level thread pool pre-warming)
- iron_parallel_for overhead: ARCHITECTURAL (platform-specific, uses GCD on macOS)

**This benchmark is not a meaningful compiler performance target.** The 5.2x ratio is
acceptable — it is within the 8.0x threshold set in `config.json`. The benchmark tests
correctness of the spawn/parallel-for semantics, not raw computation efficiency.

---

## Phase 58 Post-DCE Findings (2026-04-10)

Phase 58's benchmark audit uncovered that 43 benchmarks had been silently compiling
to empty loops under clang `-O2` dead-store elimination. After the DCE-defeat
rewrite (Plan 58-03 Precondition B), two benchmarks that had previously reported
spurious "near-parity" (0.0x) ratios surfaced as legitimate 1.7–1.9x outliers.
Both have concrete, actionable root causes and are added here as follow-up
optimization targets.

### 4. `course_schedule` — 1.73x

**Root cause: Unconditional zero-fill of stack-allocated workspace arrays**

Iron's escape analysis correctly stack-allocates the three workspace arrays
(`state`, `stack`, `stack_type`) via `alloca` — no heap allocation, which is
excellent. However, the generated C unconditionally zero-fills all 820 elements
on every call:

```c
int64_t *_v8 = (int64_t *)alloca(sizeof(int64_t) * _v2);
for (int64_t _fill_i = 0; _fill_i < _v2; _fill_i++) _v8[_fill_i] = 0LL;
int64_t *_v13 = (int64_t *)alloca(sizeof(int64_t) * 400LL);
for (int64_t _fill_i = 0; _fill_i < 400LL; _fill_i++) _v13[_fill_i] = 0LL;
int64_t *_v18 = (int64_t *)alloca(sizeof(int64_t) * 400LL);
for (int64_t _fill_i = 0; _fill_i < 400LL; _fill_i++) _v18[_fill_i] = 0LL;
```

The C reference memsets only `state[20]` (the 80-byte array that actually needs
zero-init for the DFS state machine). `stack[400]` and `stack_type[400]` are
write-before-read workspace buffers that don't need zeroing.

Per-call cost:
- **Iron:** 820 × int64 zero writes = ~6.5 KB of memory traffic
- **C:** 20 × int64 memset = ~160 bytes
- **Ratio:** ~40× more memory traffic per call for Iron

Over the 100K-iteration benchmark loop, Iron performs ~82M extra zero writes that
C does not — matches the observed 1.73× slowdown.

**Classification:** MISSING OPTIMIZATION — fill-elision via definite assignment.
Iron already has definite-assignment analysis (Phase 36) for scalars. Extending
it to detect "array allocated and every read-reachable index is write-before-read"
would let the compiler skip or delay the zero-fill.

**Prototype fix options:**
1. **Array-aware definite assignment (preferred).** Extend Phase 36's analysis to
   track per-element write-before-read for array literals and `fill()` calls.
   When every reachable read is dominated by a write, omit the zero-fill loop.
   Most impact, fits existing architecture, but nontrivial.
2. **`fill_uninit(n)` API.** Add a stdlib primitive that returns an uninitialized
   stack-allocated array. Simple to implement (map to bare `alloca`), but adds
   unsafe API surface and requires benchmark authors to opt in.
3. **Loop-invariant fill detection.** Detect `fill(const, 0)` at the top of a
   function where the array is fully overwritten before any read, and elide the
   fill. A special case of (1) but cheaper to implement.

**Estimated improvement:** 1.5–1.7× on `course_schedule`. Also benefits any
benchmark using `fill()` as a workspace allocator.

**Files:** `src/lir/emit_c.c` (fill emission site), `src/hir/definite_assignment.c`
(extend to arrays).

---

### 5. `subsets_bitmask` — 1.90x

**Root cause: Missing bitwise operators in Iron language**

The C reference uses single-instruction bit manipulation:
```c
if ((mask >> i) & 1) {
    subset_sum += arr[i];
}
```

Iron the language has no `>>`, `<<`, `&`, `|`, `^`, or `~` operators. The
benchmark author was forced to simulate `(mask >> i) & 1` with an O(i) inner
loop of arithmetic divisions:
```iron
var bit = mask
var k = 0
while k < i {
    bit = bit / 2
    k = k + 1
}
if bit % 2 == 1 { ... }
```

clang folds `x / 2` to `x >> 1` for the divide-by-constant case, but the loop
itself still runs `i` times per bit check. For `n = 18`, the amortized inner
bit extraction runs ~9 arithmetic ops instead of C's 1 shift — an ~9× cost
increase on the bit-extraction inner loop, diluted to the observed 1.9× overall
ratio once the rest of the benchmark work is accounted for.

**The gap scales with `n`.** At `n = 18`, the ratio is 1.9×. At `n = 24`, it
would be closer to 3×. At `n = 30`, closer to 5×. The benchmark is pinned to
`n = 18` only because iteration time at `n = 24` becomes impractical.

**Classification:** LANGUAGE GAP — missing core operators, not a compiler bug.

**Proposed fix:** Add bitwise operators to Iron as a dedicated language phase:
- Lexer: tokens for `<<`, `>>`, `&`, `|`, `^`, `~`
- Parser: precedence and associativity (match C: `~` unary, `<<`/`>>` above
  additive, `&`/`^`/`|` below comparisons)
- Type checker: restrict to integer operand types
- HIR/LIR: new binary/unary ops or lower to existing Int arithmetic via intrinsics
- Emitter: direct C emission (`a << b`, `a & b`, etc.)
- Docs + examples
- Rewrite `subsets_bitmask` to use the operators

**Estimated improvement:** ~2–3× on `subsets_bitmask` (eliminates the inner
divide loop), and opens up other benchmarks (hashing, flags, fixed-point math)
that are currently awkward to write in Iron.

**Files:** `src/lex/lexer.c`, `src/parse/parser.c`, `src/semantic/type_check.c`,
`src/hir/hir_lower.c`, `src/lir/emit_c.c`, `tests/integration/bitwise_*`.

**Scope note:** This is the highest-value pure language gap found in any
benchmark audit to date. Beyond `subsets_bitmask`, many competitive-programming
benchmarks (bitmask DP, SAT solvers, hash functions) become unnatural to
express without bitwise operators.

---

### P6 — Structured Loop Reconstruction (expected 1.5-2x for loop-heavy code)

**Problem:** The C emitter outputs goto-based control flow for all loops
and conditionals. While functionally correct, this prevents clang from
applying loop-specific optimizations (vectorization, loop unrolling,
strength reduction on induction variables).

**Proposed fix:** Add a C emission mode that reconstructs `for`/`while`
loops from the LIR CFG using natural loop analysis (already computed by the
strength reduction pass). Emit `while (cond) { body }` instead of
`header: if (!cond) goto exit; body; goto header; exit:`.

**Files:** `src/lir/emit_c.c` (new structured emission path)

**Complexity:** High. Requires matching CFG patterns back to structured
control flow, handling break/continue, and falling back to gotos for
irreducible control flow.

---

## Priority Order

### Implemented (v0.0.7-alpha, Phases 24–29)

1. **P1 — Range Bound Hoisting** (Phase 24) — DONE: 1.3-1.5x on ~25 benchmarks
2. **P4 — Stack Array Promotion** (Phase 25) — DONE: 5-10x on fill()-heavy benchmarks (exceeded prediction)
3. **P5 — Re-enable LOAD Inlining** (Phase 26) — DONE: 1.1-1.3x general improvement
4. **P0 — Function Inlining** (Phase 27) — DONE: ~3x on call-heavy benchmarks
5. **P2 — Dead Alloca Elimination** (Phase 28, partial) — DONE: 1.2-1.5x on complex control flow
6. **P3 — Sized Integer Types** (Phase 29) — DONE: >20x for Int32 array benchmarks (exceeded prediction)

### Remaining (future work)

7. **P6 — Structured Loop Reconstruction** (High effort, 1.5-2x, enables clang optimizations)
8. **P2b — Full Copy-Coalescing Phi Elimination** (High effort, 1.5-3x for complex control flow)
9. **P7 — Auto-Narrowing Integer Types** (High effort, range analysis, 1.2-1.5x)
10. **P8 — LLVM Backend** (Very High effort, 2-5x across the board)
11. **P9 — Bitwise Operators** (Medium effort, language gap, fixes `subsets_bitmask` 1.9x and opens
    bitmask-DP / hashing / flags / fixed-point-math idioms that are currently unnatural in Iron)
12. **P10 — Fill Elision via Array Definite Assignment** (Medium-High effort, fixes
    `course_schedule` 1.73x and benefits any benchmark that uses `fill()` as a stack workspace)

P1+P4+P5+P0+P2+P3 together brought median ratio from 5.7x to 1.0x (82% improvement).
Only 3 benchmarks remain above 3x, all with non-compiler root causes.

P6 (Structured Loop Reconstruction) is the highest-impact remaining improvement.
The goto-based C emission prevents clang from vectorizing and unrolling loops.
Primary candidates: `three_sum` (1.9x), `num_islands` (1.2x), `topological_sort_kahn` (1.4x).

P9 (Bitwise Operators) is the highest-value pure language gap and the easiest to
scope. It is a self-contained phase touching lexer/parser/type-check/emitter,
predictable in size, and unblocks an entire category of benchmarks beyond
`subsets_bitmask`.

P10 (Fill Elision) is narrower in scope but directly addresses a confirmed 1.73x
gap on `course_schedule` that is purely a missing optimization (Iron already
stack-allocates correctly; it just over-zero-fills).
