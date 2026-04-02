# Stack Research

**Domain:** Compiled programming language — lambda capture system for a C-targeting transpiler
**Researched:** 2026-04-02
**Confidence:** HIGH (analysis grounded directly in the Iron codebase and the 20 canonical C output examples in docs/lambda-capture/)

---

## Overview

This document covers the **additions and changes** to Iron's compiler stack required to implement full lambda capture (all 20 canonical patterns). It does not re-research anything already working. Every recommendation is tied to specific files in the compiler and specific examples from `docs/lambda-capture/`.

The fundamental gap: Iron's current MAKE_CLOSURE instruction emits `void* _v# = (void*)func_name` — a raw function pointer with no environment. No capture analysis pass exists. Lifted lambda functions take no `void *env` parameter. The 20 examples need a complete closure representation.

---

## Current State Audit

Understanding what exists before prescribing changes:

| Area | Current State | Gap |
|------|--------------|-----|
| Lambda lifting | Works: AST lambda → lifted `__lambda_N` top-level HIR func | Lifted functions have no `env` param; they can't access outer variables |
| MAKE_CLOSURE (LIR) | Emits `void* _vN = (void*)func_name` | No env struct allocated; `capture_count` is always 0 because no capture analysis feeds it |
| Closure call sites (LIR CALL) | Resolves callee name and emits `func_name(args)` | Does not pass env; does not handle indirect calls through a fat pointer |
| Closure type representation | `void*` for any closure value | No fat pointer; cannot represent `{fn, env}` pairs as values passed across function boundaries |
| Capture analysis | Not implemented | No pass identifies which outer-scope variables a lambda reads/mutates |
| Env struct typedef | Not emitted | The `__lambda_N_env_t` struct typedef must be emitted in the C output before any function that uses it |
| Env lifetime | Not managed | Escaping closures (examples 5, 10, 11, 18, 19) need heap-allocated envs; non-escaping closures (examples 1–4, 7, 12–14) can use the same |
| Shared-state closures | Not implemented | Two closures sharing a variable (example 11) require both to hold a pointer to a heap-allocated cell, not two copies |
| Concurrency captures | Partially broken | spawn/pfor env building exists for pfor (start/end range) but no outer variable capture wiring |

---

## Recommended Stack

### Core Technologies

No new external libraries are needed. Everything required is implemented from scratch in C11 inside the existing compiler source tree. The recommended changes are architectural, not dependency additions.

| Component | Location | Purpose | Why This Approach |
|-----------|----------|---------|-------------------|
| Capture analysis pass | New file: `src/analyzer/capture.c` + `capture.h` | Walk the AST of each lambda body and identify every reference to outer-scope variables | Must run after name resolution and typechecking (which set `resolved_sym` on every `Iron_Ident`), and before HIR lowering. HIR lowering needs the capture set to wire env fields. |
| Fat pointer closure type | Extend `src/analyzer/types.h` and `src/hir/hir.h` | Represent `(fn_ptr, env_ptr)` pairs as a first-class value type | C-targeting compilers (Clang, GCC, TCC, Chibi-C, cproc) universally use `{void (*fn)(void*,...), void *env}` structs as the closure representation. This is the only representation that allows closures to be stored in collections, passed as arguments, and returned from functions without the callee knowing which specific lambda was created. |
| Env struct generation | Extend `src/lir/emit_c.c` | Emit `typedef struct { T _cap0; T _cap1; ... } __lambda_N_env_t;` before function bodies | Struct typedef must appear before the function prototype that references it. Currently the emitter has a `struct_bodies` section that is emitted early — env structs join this section. |
| Env lifetime strategy | `src/lir/emit_c.c` + `src/analyzer/capture.c` | Decide malloc vs stack allocation for each closure's environment | See "Capture-by-Value vs Capture-by-Reference" section. |
| Indirect call emission | Extend `src/lir/emit_c.c` IRON_LIR_CALL handler | Emit `closure.fn(closure.env, args...)` instead of `func_name(args)` when callee is a closure-typed variable | Currently CALL only handles direct calls and FUNC_REF. Any call through a `func(...)` typed variable must go through the fat pointer. |

### Supporting Libraries (None New)

All implementation uses existing vendored infrastructure:

| Library | Already Used | Role in Capture |
|---------|-------------|-----------------|
| stb_ds.h | Yes (`src/vendor/`) | Dynamic arrays for capture sets, hash maps for name→capture-field mapping |
| Arena allocator | Yes (`src/util/arena.h`) | Allocate capture analysis results; lifetime matches HIR module arena |
| Iron_Rc (ref-counting) | Yes (`src/runtime/iron_rc.c`) | NOT used for closure environments — see "What NOT to Add" section |

---

## Closure Representation: Fat Pointer

**Use `{fn_ptr, env_ptr}` fat pointer structs, not single function pointers.**

Every closure value that has captures must be represented as two words: a function pointer and a pointer to the environment. This is the industry-standard approach for C-targeting compilers.

The canonical C output in `docs/lambda-capture/05-nested-lambda-capture.md` already specifies this:

```c
typedef struct {
    void *env;
    int64_t (*fn)(void *, int64_t);
} closure_int_t;
```

Iron needs a family of these struct typedefs, one per function signature shape. Two strategies exist:

**Option A: Monomorphic closure typedefs (recommended)**
Generate one closure struct typedef per distinct signature. `func() -> Void` → `IronClosure_void_t`, `func(Int) -> Int` → `IronClosure_int_int_t`, etc. The emitter generates these in the `struct_bodies` section.

**Option B: Uniform `{void*, void*}` with casts**
Use a single `typedef struct { void *fn; void *env; } IronClosure_t;` everywhere and cast at call sites. Simpler to emit but loses C type safety and requires extra casts that `clang -Wall -Werror` may warn about.

**Choose Option A.** The C output files already use monomorphic closure types in the examples. The emitter can generate these from `Iron_Type` (which carries `IRON_TYPE_FUNC` with param/return types). Option A produces cleaner generated C, works with `-Wall -Werror`, and matches what's documented in the lambda-capture examples.

Implementation: extend `emit_type_to_c()` in `emit_c.c` to emit and register closure struct typedefs. Use a hash map from Iron_Type* → emitted typedef name to avoid duplicates.

---

## Capture Analysis Pass

**A new pass, `iron_capture_analyze()`, runs after typechecking and before HIR lowering.**

**What it does:**

For every lambda expression in the AST:
1. Walk the lambda body recursively.
2. For every `Iron_Ident` node, check `resolved_sym->scope_depth` against the lambda's enclosing scope depth. If the symbol is from an outer scope, it is a captured variable.
3. Classify each captured variable as **by-value** or **by-reference** (see next section).
4. Record the capture set on the `Iron_LambdaExpr` node.

**Where it attaches results:**

Add a `capture` array to `Iron_LambdaExpr` in `ast.h`:

```c
typedef struct {
    const char   *name;       /* outer variable name */
    Iron_Symbol  *sym;        /* resolved symbol */
    Iron_Type    *type;       /* resolved type */
    bool          by_ref;     /* true = store pointer to outer var; false = copy value */
} Iron_CaptureEntry;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;         /* IRON_NODE_LAMBDA */
    Iron_Type    *resolved_type;
    Iron_Node   **params;
    int           param_count;
    Iron_Node    *return_type;
    Iron_Node    *body;
    /* Filled by capture analysis pass: */
    Iron_CaptureEntry *captures;
    int                capture_count;
} Iron_LambdaExpr;
```

**Pipeline placement:**

```
AST
  → iron_resolve()      (sets resolved_sym on every ident)
  → iron_typecheck()    (sets resolved_type on every ident)
  → iron_escape_analyze()
  → iron_capture_analyze()   ← NEW: fills captures[] on every lambda
  → iron_concurrency_check()
  → iron_hir_lower()    (reads captures[] to build env params)
```

**Nested lambdas:**

When a lambda is nested inside another lambda, and the inner lambda captures a variable from the outermost scope, the outer lambda must also capture that variable (because the outer lambda's env becomes the source). This is the "capture propagation" problem. The analysis pass handles it bottom-up: process innermost lambdas first, then propagate upward.

---

## Capture-by-Value vs Capture-by-Reference

**Rule: capture `val` as value, capture `var` as pointer-to-outer.**

This maps precisely to the canonical C output examples:

- Example 01 (immutable string capture): `const char *name` stored by value in env — correct for `val name`.
- Example 02 (mutable count): `int64_t *count` pointer in env — correct for `var count` so mutations are visible at the call site.
- Example 05 (escaping adder): captures `x` by value because `x` is a parameter (immutable in Iron).
- Example 10 (escaping counter): `n` lives in the env struct itself (not a pointer-to-stack) because it outlives the frame.
- Example 11 (shared state): both closures hold `int64_t *count` pointing to a heap-allocated cell — the cell is the shared source of truth.

**Decision tree for each captured variable:**

```
Is the variable declared `val`?
  YES → capture by value (copy into env field)
  NO (var or mutable parameter):
    Does either closure escape the declaring scope?
      YES (closure is returned, stored in heap, or passed to a longer-lived scope):
        Is there more than one closure sharing this var?
          YES → allocate a heap cell; all closures hold a pointer to the cell
          NO → copy the variable's current value into the env at construction time
               and copy back on return (only works for non-escaping single closures)
        Preferred: always use heap cells for mutable captures — simpler, no copy-back
      NO (closure does not escape):
        Capture by pointer to the stack variable — no malloc needed
```

**Practical simplification for Iron v0.1.0:**

Use **always-by-pointer for `var` captures**:
- Non-escaping closures: store `&outer_var` in env (stack pointer, no malloc for the captured value itself).
- Escaping closures: the captured `var` must be promoted to a heap cell. The capture analysis pass marks a `var` as "heap-promoted" when any lambda that captures it escapes.

This matches what all 20 examples demonstrate. It avoids needing a copy-back mechanism, which is error-prone.

**Closures capturing object references (examples 6, 20):**

When a `var state: GameState` is captured, the env stores `GameState *state`. This is already the correct pattern — capturing an object always captures by pointer because Iron objects are value types by default but closures need to see mutations.

---

## Environment Struct Generation

**The env struct is emitted as a `typedef struct` in the `struct_bodies` section of the C output.**

For a lambda `__lambda_3` that captures `int64_t count` (by pointer) and `const char *label` (by value):

```c
typedef struct {
    int64_t     *_cap0;   /* count (mutable, by pointer) */
    const char  *_cap1;   /* label (immutable, by value) */
} __lambda_3_env_t;
```

The lifted function signature becomes:

```c
int64_t __lambda_3(void *_env, /* explicit params */ int64_t y);
```

At the call site (MAKE_CLOSURE):

```c
__lambda_3_env_t *_env_42 = (__lambda_3_env_t *)malloc(sizeof(__lambda_3_env_t));
_env_42->_cap0 = &count;
_env_42->_cap1 = label;
IronClosure_int_int_t _v42 = { .fn = __lambda_3, .env = _env_42 };
```

**Where env struct typedefs are emitted in the existing C output pipeline:**

The existing `EmitCtx` in `emit_c.c` has an `Iron_StrBuf struct_bodies` that is written to the C output before function prototypes (see the `iron_lir_emit_c` phase ordering). Env struct typedefs join this section. They must be emitted here, not inline in function bodies, because:
1. Multiple functions may reference the same closure type.
2. Closure struct typedefs reference env struct typedefs.
3. C requires `typedef struct` to precede any use.

**Concrete change to emit_c.c:**

Add a new phase (Phase 2b) that iterates over all MAKE_CLOSURE instructions across all functions, generates env struct typedefs, and appends them to `struct_bodies`. Run this phase after `emit_type_decls()` but before `emit_func_signature()` calls.

---

## Lifetime Management for Environments

**Non-escaping closures: malloc+free at function scope boundary.**

For closures that do not escape (examples 1, 2, 3, 4, 7, 8, 12, 13, 14):
- Allocate env with `malloc` at MAKE_CLOSURE site.
- Free env when the containing function returns or when the closure variable goes out of scope.

The escape analysis pass already marks `Iron_HeapExpr` nodes with `auto_free`. The same logic applies to env structs: if the closure does not escape, emit `free(_env_N)` before the function return.

**Escaping closures: caller is responsible for freeing.**

For closures returned from functions (examples 5, 10, 18, 19) or stored in collections (example 8, 17):
- The env is allocated at MAKE_CLOSURE and ownership is transferred with the closure value.
- The caller that holds the closure is responsible for `free(closure.env)`.

**Iron v0.1.0 approach: always malloc, rely on compiler-generated free for non-escaping case.**

Do not use stack allocation for envs in v0.1.0. Stack-allocated envs create dangling pointer problems when closures escape (easy to get wrong). Always `malloc`. The auto-free extension to MAKE_CLOSURE handles the non-escaping case automatically.

Do NOT use Iron's `Iron_Rc` ref-counting for environments. Rc adds overhead (atomic increments/decrements) and complexity (destructor registration). For closure envs, the ownership model is deterministic — one owner at a time in most cases, or explicit shared-cell for shared-state closures. Plain `malloc`/`free` is sufficient and matches the canonical C output examples exactly.

**Shared-state closures (example 11): heap-allocated cell, not ref-counted.**

When two closures share a mutable variable, the pattern is:

```c
int64_t *count = malloc(sizeof(int64_t));
*count = 0;
inc_env->count = count;   /* both envs point to the same cell */
get_env->count = count;
/* ... */
free(inc_env);
free(get_env);
free(count);              /* freed once, after both envs are freed */
```

This is what the canonical C output shows. No ref-counting. The compiler must emit the shared cell allocation and the final free. In v0.1.0 this can be driven by the "heap-promoted" flag on captured `var` declarations: if a `var` is heap-promoted, allocate it as a separate cell and emit its free after all closures that capture it are freed.

---

## Thread Safety for Concurrent Captures

**Spawn captures (example 15): pass by-copy for all captures.**

When a spawn or parallel-for captures outer variables, the task body runs concurrently. The safe strategy for v0.1.0:
- Immutable captures (`val`): pass by value (copy into task env) — no synchronization needed.
- Mutable captures (`var`): permitted only for index-disjoint writes (like example 16 where each iteration writes to its own index). General mutable shared state across spawn boundaries requires a Mutex — the concurrency checker already enforces this.

The existing `iron_concurrency_check()` in `src/analyzer/concurrency.c` already prevents parallel-for from mutating outer `var` variables. This constraint applies equally to captures in spawn blocks: a spawn body may not write to a captured outer `var` without a Mutex. The capture analysis pass feeds the concurrency checker with the capture set, and the concurrency checker validates it.

**Parallel-for captures (example 16): pass both arrays by pointer.**

The existing pfor lowering in `hir_lower.c` creates a chunk function with `(start, end)` parameters for the range only. Example 16 requires that outer arrays (`weights`, `results`) are also accessible in the chunk function. These are passed by pointer in the chunk env:

```c
typedef struct {
    const double *weights;
    double       *results;
    int64_t       start;
    int64_t       end;
} __pfor_0_ctx_t;
```

This matches `docs/lambda-capture/16-parallel-for-readonly.md` exactly. The pfor case is a special instance of the general capture problem: the pfor body is a "closure" whose env contains all captured outer variables plus the range bounds.

**No `_Atomic` or mutex for v0.1.0 read-only captures.**

Reading outer `val` arrays from multiple threads simultaneously is safe without synchronization. The capture system passes a pointer to the array (not a copy) because arrays are too large to copy. `const` annotation on the pointer documents read-only intent. This is correct C11 — concurrent reads without writes are safe.

---

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| Fat pointer `{fn, env}` per signature | Single opaque `{void*, void*}` everywhere | Requires casts that break `-Wall -Werror`. The canonical C output examples use typed fat pointers. Uniform opaque approach makes the generated C harder to debug and loses type information at call sites. |
| `malloc` for all env structs | Stack-allocated envs for non-escaping case | Stack envs are an optimization that can be added later. For v0.1.0, always-malloc is correct and simpler. A stack-allocated env that escapes (even accidentally) causes a dangling pointer — hard to debug. |
| Heap cell for shared mutable captures | Ref-counted env struct shared between two closures | Rc adds atomic overhead. The shared-cell pattern (allocate one `int64_t*`, point both envs at it) is simpler, faster, and matches the documented C output. |
| Capture analysis in new `capture.c` | Extend the existing `resolve.c` | Resolution and capture analysis are separate concerns. Capture analysis requires all types to be resolved first (it reads `resolved_type`). Mixing them would create ordering dependencies inside the resolver. |
| Capture analysis annotates AST `Iron_LambdaExpr` | Store capture info only in HIR | AST annotation allows the concurrency checker (which operates on AST) to validate captures. HIR-only annotation would require duplicating the concurrency checker logic in a new HIR-level pass. |
| Always-by-pointer for mutable captures | Copy-in/copy-out for non-escaping mutable captures | Copy-in/copy-out is correct only when there is a single closure and it does not escape. The general case (multiple closures, escaping) breaks. By-pointer is always correct and simpler to implement. |

---

## What NOT to Add

| Avoid | Why | What to Use Instead |
|-------|-----|---------------------|
| GC or tracing garbage collection | Iron explicitly uses manual memory management. Adding GC for closures would break the rest of the memory model and create a split-brain situation. | malloc/free with deterministic lifetimes derived from escape analysis. |
| Boxing all captured values | Boxing every captured variable (allocating a heap cell for each independently, even when no sharing occurs) wastes memory and adds pointer-chasing. Only shared mutable state needs heap cells. | Value capture for `val`, pointer-to-stack for non-escaping `var`, heap cell only when multiple closures share a `var` or when the `var`'s closure escapes. |
| Iron_Rc for env structs | Iron_Rc uses atomic reference counting. For closure envs with deterministic ownership (one closure owns its env), Rc adds overhead with no benefit. Even for shared-state closures (example 11), the raw shared-cell pattern is sufficient. | Plain malloc/free. |
| A separate HIR "closure conversion" pass that rewrites the entire AST | Full closure conversion (CPS transform, lambda lifting with explicit substitution) is used in functional language compilers (OCaml, Haskell). Iron's backend already lifts lambdas to top-level functions. The remaining work is just wiring the env parameter — no CPS transform needed. | Extend the existing lift infrastructure in `hir_lower.c` to pass the env as a `void *` parameter and read from it at captured-variable use sites. |
| C++ lambda semantics (`[=]` copy-all, `[&]` ref-all) | Iron's capture is always explicit in the semantics (the compiler infers what's captured from the lambda body). No syntax change needed. Do not add capture-clause syntax to the Iron language. | Implicit capture: the compiler infers the capture set automatically. |
| Separate `iron_closure` runtime library | A small set of closure macros or inline functions in `iron_runtime.h` is sufficient. There is no need for a separate library with its own initialization. | Emit closure typedefs directly in the generated C output, with any helper macros in `iron_runtime.h`. |

---

## Implementation Strategy by Pipeline Stage

This maps each required change to the correct file and explains why that file, not another.

### Stage 1: Capture Analysis (New Pass)

**File: `src/analyzer/capture.c` + `src/analyzer/capture.h`**

Runs after `iron_typecheck()`. Walks every `Iron_LambdaExpr` in the AST recursively. For each `Iron_Ident` in the lambda body, checks if `resolved_sym->depth` is outside the lambda's scope depth. Records all outer-scope references as captures. Marks `var` captures that require heap promotion (when the lambda escapes its declaring scope). Attaches results to `Iron_LambdaExpr.captures[]`.

Called from `src/analyzer/analyzer.c` between `iron_typecheck()` and `iron_concurrency_check()`.

### Stage 2: HIR Lowering — Env Param Wiring

**File: `src/hir/hir_lower.c`**

In `lower_lift_pending_hir()`, the LIFT_LAMBDA case currently builds a lifted function with only the explicit lambda parameters. It must be extended to:
1. Add a `void *_env` parameter as the first parameter of the lifted function.
2. At the start of the lifted function body, emit `let` instructions that unpack env fields into local variables: `let count_ref: *Int = (__lambda_N_env_t *)_env->_cap0;`
3. Replace uses of captured variable names inside the lambda body with references to the unpacked env fields.

In the HIR closure expression, the `captures` field (currently `NULL, 0`) must be populated from `Iron_LambdaExpr.captures[]`.

### Stage 3: LIR — Capture Population

**File: `src/hir/hir_to_lir.c`**

The `IRON_HIR_EXPR_CLOSURE` case currently emits `iron_lir_make_closure(ctx, lifted_name, NULL, 0, ...)`. It must be extended to iterate over the closure's capture list and emit LOAD instructions for each captured value, then pass those value IDs as the `captures` array to `iron_lir_make_closure`.

### Stage 4: C Emission — Env Structs and Fat Pointers

**File: `src/lir/emit_c.c`**

Four changes required:

1. **Phase 2b (new):** Before emitting function prototypes, scan all MAKE_CLOSURE instructions across all functions. For each closure with captures, emit the env struct typedef into `struct_bodies`. Emit the monomorphic closure struct typedef (e.g., `IronClosure_void_t`) into `struct_bodies` if not already emitted.

2. **MAKE_CLOSURE instruction handler:** Currently emits `void* _vN = func_name`. Must emit: (a) allocate env struct, (b) populate fields, (c) construct fat pointer: `IronClosure_X_t _vN = { .fn = __lambda_N, .env = _env_N };`

3. **CALL instruction handler:** When callee is a closure-typed variable (not a direct function reference), emit `_vN.fn(_vN.env, args...)` instead of `func_name(args...)`. Detect this case by checking the type of the callee value.

4. **Function signature emitter:** Lifted lambda functions now have a `void *_env` first parameter. The emit_func_signature must emit this for all `__lambda_N` functions that have captures.

### Stage 5: Concurrency Check Extension

**File: `src/analyzer/concurrency.c`**

Extend `iron_concurrency_check()` to validate spawn block captures using the capture sets produced by the new capture analysis pass. A spawn body that captures a mutable outer variable (other than index-disjoint array writes) must emit E0208 or a new E0209 "mutable capture in spawn requires mutex".

---

## Closure Performance Considerations

The canonical C output examples use `malloc` for all env structs. This is correct and sufficient for v0.1.0. Performance notes for future optimization work (out of scope for v0.1.0):

- **Stack-allocated envs for non-escaping closures:** If escape analysis confirms the closure does not outlive the current function, the env can be stack-allocated (`__lambda_N_env_t env_storage; env_storage._cap0 = ...`). This eliminates the malloc/free pair entirely. The existing `auto_free` / `escapes` flags on `Iron_HeapExpr` provide the model to extend to env allocation.
- **Small-closure optimization:** If the closure captures zero variables, no env needed — already handled by the current emitter's `capture_count == 0` check.
- **Inlining:** The existing function inlining pass already inlines small functions at LIR level. Closures with small bodies and value-only captures are candidates for inlining, which eliminates the fat pointer dispatch entirely.

---

## Version Compatibility

All changes are internal to the compiler. No new external dependencies. The generated C must continue to compile with `-std=c11 -Wall -Werror` on both clang and gcc.

Env struct typedefs and closure typedefs use only C11-compatible constructs: `typedef struct { ... }`, designated initializers (`.fn = x, .env = y`), and `void *` casts. All 20 canonical C output examples use only these constructs.

---

## Sources

- `docs/lambda-capture/01-capture-single-immutable.md` through `20-game-loop-shared-state.md` — all 20 canonical C output patterns; HIGH confidence (authored alongside the compiler).
- `src/lir/emit_c.c` — direct audit of MAKE_CLOSURE handler, CALL handler, and env struct generation code; HIGH confidence.
- `src/hir/hir_lower.c` — direct audit of LIFT_LAMBDA case showing lifted functions lack env param; HIGH confidence.
- `src/hir/hir.h` — confirmed `closure.lifted_name` exists but `captures` array is present with zero count in all existing tests; HIGH confidence.
- `src/analyzer/escape.c` — confirmed escape analysis model (auto_free, escapes flags) provides the foundation for env lifetime decisions; HIGH confidence.
- `src/analyzer/concurrency.c` — confirmed existing concurrency checks operate at AST level and can be extended with capture set data; HIGH confidence.
- CPython's closure implementation (cell objects) and GCC's nested function trampolines — industry precedent for shared-mutable-state heap cells; MEDIUM confidence (not directly verified against current sources, but the pattern is stable across decades of compiler literature).

---
*Stack research for: Iron compiler — lambda capture system (v0.1.0-alpha)*
*Researched: 2026-04-02*
