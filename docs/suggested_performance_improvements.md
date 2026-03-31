# Suggested Performance Improvements

This document catalogs known performance gaps between Iron-compiled programs and
equivalent hand-written C, identifies root causes, and proposes concrete compiler
improvements ranked by expected impact.

The analysis is based on the v0.0.5-alpha benchmark suite (137 benchmarks) run
against the two-level IR pipeline (AST -> HIR -> LIR -> C -> clang).

---

## Current State

Most benchmarks run within 1.5x of C. A handful show larger gaps due to
codegen quality rather than algorithmic differences. The worst case
(`connected_components`) is ~167x slower after fixing a redundant path
compression in the Iron source.

### Overhead Breakdown (connected_components case study)

| Factor | Estimated Impact | Cumulative |
|---|---|---|
| No function inlining | 2-3x | 2.5x |
| SSA phi artifact overhead | 5-7x | 17x |
| int64_t vs int32_t type width | 1.5-2x | 30x |
| Iron_range() evaluated every iteration | 1.3-1.5x | 42x |
| Generated C structure (goto spaghetti vs structured loops) | 2-4x | 125x |

These factors multiply. A benchmark that hits all of them sees >100x overhead;
one that hits only type width and range overhead stays under 2x.

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

For maximum impact with minimum effort:

1. **P1 — Range Bound Hoisting** (Low effort, 1.3-1.5x, affects all for-loops)
2. **P0 — Function Inlining** (Medium effort, 2-3x, affects call-heavy benchmarks)
3. **P4 — Stack Array Promotion** (Low-Medium effort, 1.2-1.5x, affects fill()-heavy code)
4. **P5 — Re-enable LOAD Inlining** (Medium effort, 1.1-1.3x, general improvement)
5. **P2 — Improved Phi Elimination** (High effort, 3-5x, affects complex control flow)
6. **P3 — Sized Integer Types** (Medium effort, 1.5-2x, language-level change)
7. **P6 — Structured Loop Reconstruction** (High effort, 1.5-2x, enables clang optimizations)

Implementing P0+P1+P4 alone would close the gap for most benchmarks to
under 3x of C, which is competitive with other high-level-to-C compilers.
