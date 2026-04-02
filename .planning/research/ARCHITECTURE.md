# Architecture Research

**Domain:** Lambda capture integration into Iron compiler (AST -> HIR -> LIR -> C pipeline)
**Researched:** 2026-04-02
**Confidence:** HIGH — based on direct source code audit of all pipeline stages

## Standard Architecture

### System Overview

The current pipeline before capture work:

```
Iron Source
    |
    v
+------------------------------------------------------------------+
|  SEMANTIC ANALYSIS (src/analyzer/)                               |
|  resolve.c -> typecheck.c -> escape.c -> concurrency.c           |
|  . Name resolution (scope-based)                                 |
|  . Type annotation on all expression nodes                       |
|  . Heap escape marking (auto_free / escapes flags)               |
|  . Parallel mutation checking                                     |
|  GAP: no capture variable collection pass                        |
+------------------------------------------------------------------+
                               |
                               v  Iron_Program (annotated AST)
+------------------------------------------------------------------+
|  HIR LOWERING -- Pass 1/2/3 (src/hir/hir_lower.c)               |
|  . Pass 1: register func/method sigs, collect globals            |
|  . Pass 2: lower each function body to HIR stmts/exprs           |
|  . Pass 3: lift lambda/spawn/pfor bodies to top-level            |
|    HIR functions (lower_lift_pending_hir)                        |
|  GAP: IRON_HIR_EXPR_CLOSURE has no captures[] field             |
|  GAP: lifted functions have no void *_env first param            |
+------------------------------------------------------------------+
                               |
                               v  IronHIR_Module
+------------------------------------------------------------------+
|  HIR -> LIR (src/hir/hir_to_lir.c)                              |
|  . Flattens HIR structured control into basic blocks             |
|  . CLOSURE -> IRON_LIR_MAKE_CLOSURE(lifted_name, NULL, 0)        |
|    captures=NULL, capture_count=0 -- always empty               |
|  . Indirect CALL via func_ptr casts as (ret_type (*)())          |
|    with no env argument passed to lifted function                |
+------------------------------------------------------------------+
                               |
                               v  IronLIR_Module
+------------------------------------------------------------------+
|  LIR OPTIMIZATION (src/lir/lir_optimize.c)                       |
|  phi_eliminate -> array_param_modes -> array_repr ->             |
|  function_inlining -> fixpoint(copy-prop, const-fold, DCE,       |
|  store-load-elim, strength-reduction) ->                         |
|  dead_alloca_elimination -> inline_info                           |
|  MAKE_CLOSURE is pure (iron_lir_instr_is_pure = true)            |
|  captures[] already threaded through all operand-rewrite passes  |
+------------------------------------------------------------------+
                               |
                               v  IronLIR_Module (optimized)
+------------------------------------------------------------------+
|  C EMISSION (src/lir/emit_c.c)                                   |
|  . MAKE_CLOSURE: env struct malloc path only when capture_count>0 |
|    (never reached today -- capture_count is always 0)            |
|  . Env struct name derived: "{lifted_name}_env"                  |
|  . Closure stored as void* pointing to function address only     |
|  . IRON_TYPE_FUNC -> "void*" C type                              |
|  . Indirect call: (ret_type (*)())fptr (no env argument)         |
+------------------------------------------------------------------+
```

After capture work, the pipeline becomes:

```
Iron Source
    |
    v
+------------------------------------------------------------------+
|  SEMANTIC ANALYSIS -- EXTENDED                                   |
|  NEW: capture_analysis.c (runs after typecheck)                  |
|  . Walk each lambda body collecting free-variable refs           |
|  . Annotate Iron_LambdaExpr with:                               |
|      CaptureVar captures[]  (name, type, is_mutable, escapes)   |
|  . Distinguish value vs pointer captures by mutability           |
|  . Detect escaping closures (returned or stored to heap)         |
|  . Detect shared captures (same var in multiple sibling lambdas) |
+------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------+
|  HIR LOWERING -- EXTENDED                                        |
|  . IRON_HIR_EXPR_CLOSURE: add captures[] + capture_count        |
|  . Pass 3 (lower_lift_pending_hir):                              |
|    - emit lifted func with void *_env as first parameter         |
|    - inside body, rewrite captured var refs as env->field access |
|  . Shared captures (ex 11): allocate shared heap cell once;     |
|    each closure env stores pointer to the same heap cell         |
+------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------+
|  HIR -> LIR -- EXTENDED                                         |
|  . CLOSURE -> MAKE_CLOSURE(lifted_name, captures[], N)           |
|    captures[] = LIR ValueIds (alloca addresses for mutable,     |
|    plain values for immutable)                                   |
|  . CALL of func-typed var -> fat pointer invoke:                 |
|    extract env + fn from closure struct, call fn(env, args...)   |
+------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------+
|  LIR OPTIMIZATION -- UNCHANGED (mostly)                          |
|  . MAKE_CLOSURE already pure; captures[] already in rewriter     |
|  . Function inlining: guard against inlining lifted closures     |
|    (their void *_env first param breaks the inliner model)       |
|  . Store-load-elim: env struct fields accessed through pointer;  |
|    allocas passed to MAKE_CLOSURE captures[] must be marked      |
|    escaped so their values are not forwarded past the closure    |
+------------------------------------------------------------------+
                               |
                               v
+------------------------------------------------------------------+
|  C EMISSION -- EXTENDED                                          |
|  NEW: env_typedefs StrBuf section (before lifted_funcs)          |
|  . Typed env struct typedef with named fields per closure:       |
|    immutable: const char *name  (value copy)                     |
|    mutable:   int64_t *count    (pointer for writeback)          |
|  . Escaping closure: env malloc'd in enclosing func              |
|  . IRON_TYPE_FUNC -> Iron_Closure struct {void *env; fn fn;}     |
|  . Indirect call: f.fn(f.env, explicit_args...)                  |
+------------------------------------------------------------------+
```

### Component Responsibilities

| Component | Current Responsibility | Capture Extension |
|-----------|----------------------|-------------------|
| `analyzer/resolve.c` | Name resolution, scope attachment | No changes needed |
| `analyzer/typecheck.c` | Type annotation on all nodes | Minor: closure type stays IRON_TYPE_FUNC |
| `analyzer/escape.c` | Heap allocation escape analysis | Extend: mark closures returned/stored as escaping |
| `analyzer/concurrency.c` | Parallel mutation check | Extend: validate spawn captures are read-only |
| `analyzer/capture_analysis.c` (NEW) | -- | Free-variable collection; annotate Iron_LambdaExpr |
| `hir/hir.h` | HIR data structures | Add `captures[]` + `capture_count` to closure union |
| `hir/hir_lower.c` | AST->HIR 3-pass lift | Pass 3: add env param + rewrite captured refs |
| `hir/hir_to_lir.c` | HIR->LIR basic-block flatten | CLOSURE: pass captures[]; CALL: fat ptr pattern |
| `lir/lir_optimize.c` | All optimization passes | Inlining exclusion for lifted closures |
| `lir/emit_c.c` | LIR->C text | Env struct typedef; typed fields; fat ptr calls |

## Recommended Project Structure

No new top-level directories needed. New file fits in existing `src/analyzer/`:

```
src/
+-- analyzer/
|   +-- analyzer.c          (orchestrates semantic pipeline -- add capture_analysis call)
|   +-- resolve.c           (unchanged)
|   +-- typecheck.c         (no changes needed)
|   +-- escape.c            (extend: closure escape detection)
|   +-- concurrency.c       (extend: spawn capture safety)
|   +-- capture_analysis.c  (NEW: free-variable collection + annotation)
|   +-- capture_analysis.h  (NEW: public API)
+-- hir/
|   +-- hir.h               (extend IronHIR_Expr.closure with captures[])
|   +-- hir.c               (extend iron_hir_expr_closure constructor)
|   +-- hir_lower.c         (extend Pass 3: env param + ref rewriting)
|   +-- hir_to_lir.c        (extend CLOSURE case + indirect CALL case)
+-- lir/
    +-- lir_optimize.c      (add lifted-func guard to inliner)
    +-- emit_c.c            (env struct typedef; typed fields; fat ptr calls)
```

### Structure Rationale

- **capture_analysis.c in analyzer/:** Capture analysis is semantic analysis — it operates on the type-annotated AST, needs resolved symbols and types, and produces annotations consumed by HIR lowering. It belongs after typecheck, before HIR. Placing it in the analyzer directory keeps the pipeline stage clear.
- **No new HIR/LIR node types needed:** `IRON_LIR_MAKE_CLOSURE` already has `captures[]` and `capture_count` fields in `lir.h`. `IRON_HIR_EXPR_CLOSURE` needs `captures[]` added to `hir.h`. No new instruction opcodes.
- **No new LIR instructions:** The existing `IRON_LIR_MAKE_CLOSURE` and `IRON_LIR_CALL` can represent all 20 capture patterns when their data fields are correctly populated.

## Architectural Patterns

### Pattern 1: Capture Classification as a Dedicated Semantic Pass

**What:** A new pass `iron_capture_analyze()` runs after `iron_typecheck()` in the semantic pipeline. It walks each lambda expression, identifies variable references in the body that are not declared as lambda parameters, and records them as captures. It annotates `Iron_LambdaExpr` directly with a `CaptureVar[]` array.

**When to use:** This placement is correct for three reasons: (a) `Iron_Ident.resolved_sym` is populated by `resolve.c` so we can look up the scope depth of each referenced variable; (b) `Iron_Type` annotations from `typecheck.c` give us the correct C type per field; (c) the full scope chain is still available in the AST at this point — it is gone by the time HIR Pass 3 runs.

**Data structures to add to `parser/ast.h` (or a new `capture_analysis.h`):**
```c
typedef struct {
    const char  *name;       /* captured variable name */
    Iron_Type   *type;       /* resolved type of the variable */
    bool         is_mutable; /* true = var; false = val */
    bool         escapes;    /* lambda outlives its enclosing scope */
    bool         shared;     /* same var captured by another sibling closure */
} Iron_CaptureVar;
```

Fields added to `Iron_LambdaExpr` in `parser/ast.h`:
```c
Iron_CaptureVar  *captures;
int               capture_count;
```

### Pattern 2: Env Struct with Typed Named Fields

**What:** Each lifted lambda function gets a typed env struct emitted as a C typedef in a new `env_typedefs` section that appears before `lifted_funcs` in the C output. Fields use the actual Iron variable names and correct C types, not positional `_cap0`, `_cap1` names.

Immutable capture (val) -- captured by value:
```c
typedef struct {
    const char *name;   /* val name = "Iron" */
} __lambda_0_env_t;
```

Mutable capture (var) -- captured by pointer so mutations propagate back to the enclosing scope:
```c
typedef struct {
    int64_t *count;     /* var count = 0 */
} __lambda_1_env_t;
```

Multi-type (example 3 -- Int, Float, String):
```c
typedef struct {
    int64_t       *x;
    double        *y;
    Iron_String   *label;
} __lambda_2_env_t;
```

**Implementation:** Add a new `env_typedefs` field to `EmitCtx` (an `Iron_StrBuf`). Populate it inside the `IRON_LIR_MAKE_CLOSURE` handler in `emit_instr()`. Flush it between `struct_bodies` and `lifted_funcs` in `iron_lir_emit_c()`.

The current code derives the env struct name as `"{lifted_name}_env"` -- keep this convention but change `_capN` generic field names to the actual Iron variable names.

### Pattern 3: Fat Pointer Representation for Closure Values

**What:** A closure that carries an environment cannot be represented as a bare `void*` function pointer. It must carry both the function pointer and the environment pointer. Use a two-word "fat pointer" struct.

For uniformity, use a single generic closure struct for all `func(...)` types in C:
```c
typedef struct {
    void *env;
    void (*fn)(void *);
} Iron_Closure;
```

At call sites, cast the `fn` field to the actual signature before invoking:
```c
/* call f: func(Int) -> Int */
int64_t result = ((int64_t (*)(void *, int64_t))f.fn)(f.env, arg);
```

**Trade-offs:**
- Pro: single struct type for all closures; no per-signature struct proliferation
- Pro: matches exactly what the 20 example C outputs show (see `closure_t` in examples 7, 10, 11, 18)
- Con: call site requires explicit cast; C compiler loses static parameter type checking
- The existing code already casts function pointers for indirect calls -- this pattern is consistent

**When IRON_TYPE_FUNC appears as a C type:** Change `emit_type_to_c` to return `"Iron_Closure"` instead of `"void*"`. Forward-declare `Iron_Closure` before the first use (in the `forward_decls` section).

### Pattern 4: Shared Capture via Heap-Allocated Cells

**What:** When two sibling closures in the same scope both capture the same `var`, they must share a single heap-allocated cell so mutations by one closure are visible to the other (examples 11 and 20).

The capture analysis pass detects this: same variable name captured as mutable by two or more lambda nodes whose enclosing scope is identical. The HIR lowering for the enclosing function then:

1. Emits a heap allocation for the shared variable (replacing the stack alloca):
   `int64_t *count = malloc(sizeof(int64_t)); *count = 0;`
2. Both closures' env structs get a pointer field pointing to this heap cell.
3. All uses of the shared variable in the enclosing function's direct body also go through the pointer.

**Build order:** Implement non-shared mutable captures first (examples 2, 5, 10 -- single closure per captured var). Add shared-capture handling in a later sub-phase. Examples 1, 3, 4, 7, 9, 12, 13, 14 require neither mutable captures nor sharing.

### Pattern 5: Lifted Function Environment Parameter Convention

**What:** Every lifted lambda function gains `void *_env` as its first parameter. The function casts it to the typed env struct on entry and accesses all captured variables through the typed pointer. User-declared parameters follow.

Before (current -- no env):
```c
static int64_t __lambda_0(int64_t y) {
    /* x is unresolved -- bug */
}
```

After (with env):
```c
static int64_t __lambda_0(void *_env, int64_t y) {
    __lambda_0_env_t *e = (__lambda_0_env_t *)_env;
    return e->x + y;   /* x captured from enclosing scope */
}
```

**Zero-capture closures:** Keep the current `void*`-as-function-pointer pattern for closures with zero captures (no env param needed). This avoids breaking existing non-capturing lambda tests. For zero-capture closures passed as `func()` typed values, store as `Iron_Closure{ .env = NULL, .fn = fn }` for type uniformity.

## Data Flow

### Capture Metadata Flow

```
Iron_LambdaExpr (AST -- after resolve + typecheck)
    |
    v  (capture_analysis.c -- new pass)
Iron_LambdaExpr.captures[] populated
    | (CaptureVar: name, type, is_mutable, escapes, shared)
    |
    v  (hir_lower.c Pass 2 -- lower_expr_hir IRON_NODE_LAMBDA)
IronHIR_Expr.closure.captures[] + capture_count set
    | (mirrors AST capture annotations; HIR VarIds resolved)
    |
    v  (hir_lower.c Pass 3 -- lower_lift_pending_hir)
Lifted IronHIR_Func created with:
    . void *_env as first parameter
    . Body statements rewritten: captured var refs -> env->field
    |
    v  (hir_to_lir.c -- flatten_func + lower_expr CLOSURE case)
IRON_LIR_MAKE_CLOSURE(lifted_name, captures[], capture_count)
    . captures[] = LIR ValueIds for captured alloca addresses
    |
    v  (lir_optimize.c -- all passes)
captures[] operands rewritten by copy-prop/DCE (already handled)
    |
    v  (emit_c.c -- IRON_LIR_MAKE_CLOSURE case)
Emit: __lambda_N_env_t *_env_V = malloc(sizeof(__lambda_N_env_t));
      _env_V->count = &_vN;    /* mutable: store address */
      _env_V->name  = _vM;     /* immutable: store value */
      Iron_Closure closure_V = { _env_V, __lambda_N };
```

### Call Site Data Flow (indirect closure call)

```
func-typed variable (Iron_Closure fat pointer)
    |
    v  (hir_to_lir.c -- IRON_HIR_EXPR_CALL where callee is func-typed ident)
IRON_LIR_CALL(func_ptr=closure_val_id, args=[explicit_args...])
    | closure_val_id is MAKE_CLOSURE result
    |
    v  (emit_c.c -- IRON_LIR_CALL case, indirect path)
CURRENT (broken):  ((ret_type (*)())_vN)(args...)   // loses env
AFTER   (correct): ((ret_type (*)(void*, ...)_vN.fn)(_vN.env, args...)
```

## Integration Points

### Per-Pipeline-Stage Changes

| Stage | Component | Change Type | Scope |
|-------|-----------|-------------|-------|
| Semantic | `capture_analysis.c` (NEW) | New file | Free-variable collection; annotate Iron_LambdaExpr.captures[] |
| Semantic | `escape.c` | Extend | Detect closures returned/stored-to-heap as escaping |
| Semantic | `concurrency.c` | Extend | Validate spawn captures are not mutable outer vars |
| Semantic | `analyzer.c` | Modify (2 lines) | Call iron_capture_analyze() after iron_typecheck() |
| HIR data | `hir.h` IronHIR_Expr.closure | Add fields | Add `captures[]` + `capture_count` to closure union |
| HIR data | `hir.c` | Modify | Extend iron_hir_expr_closure() constructor to accept captures |
| HIR lower | `hir_lower.c` lower_expr_hir | Modify | Copy captures from AST annotation to HIR closure node |
| HIR lower | `hir_lower.c` lower_lift_pending_hir | Modify | Add void *_env param; rewrite captured refs in lifted body |
| HIR->LIR | `hir_to_lir.c` CLOSURE case | Modify | Pass captures[] with real LIR ValueIds to MAKE_CLOSURE |
| HIR->LIR | `hir_to_lir.c` CALL case | Modify | Indirect call to Iron_Closure: f.fn(f.env, args...) pattern |
| LIR opt | `lir_optimize.c` function_inlining | Guard | Skip inlining __lambda_N, __spawn_N, __pfor_N functions |
| LIR opt | `lir_optimize.c` store-load-elim | Verify | Allocas passed to MAKE_CLOSURE.captures[] must be escaped |
| C emit | `emit_c.c` EmitCtx | Extend | Add env_typedefs StrBuf field |
| C emit | `emit_c.c` MAKE_CLOSURE | Extend | Emit typed env struct typedef + named field stores |
| C emit | `emit_c.c` emit_type_to_c | Modify | IRON_TYPE_FUNC -> "Iron_Closure" instead of "void*" |
| C emit | `emit_c.c` CALL (indirect) | Modify | f.fn(f.env, args...) pattern; correct cast with env param |
| C emit | `emit_c.c` iron_lir_emit_c | Modify | Flush env_typedefs between struct_bodies and lifted_funcs |
| C emit | `emit_c.c` forward_decls | Extend | Forward-declare Iron_Closure typedef early in output |

### Optimization Pass Interactions

**Function Inlining (`run_function_inlining`):**
Lifted closure functions must not be inlined. The inliner clones function bodies and remaps parameter IDs based on function signature. It does not model the `void *_env` first parameter or the env-cast-then-dereference pattern inside the body. Inlining a closure body would clone the `e->field` accesses into the caller without the corresponding env setup.

Fix: add a guard at the top of the inlining candidate scan:
```c
if (strncmp(fn->name, "__lambda_", 9) == 0 ||
    strncmp(fn->name, "__spawn_",  8) == 0 ||
    strncmp(fn->name, "__pfor_",   7) == 0) continue;
```

**Copy Propagation (`run_copy_propagation`):**
The `captures[]` array in `MAKE_CLOSURE` is already handled in the operand rewriter `apply_value_replacements()` which iterates `make_closure.captures[i]` and calls `REPL()`. No changes needed.

**Dead Code Elimination (`run_dce`):**
`IRON_LIR_MAKE_CLOSURE` is classified as pure. DCE may therefore eliminate an unused MAKE_CLOSURE instruction. This is correct -- if a closure is constructed but never called, the env allocation and setup can be eliminated.

**Store-Load Elimination:**
The env struct fields are accessed through a `void *_env` pointer inside the lifted function. In the enclosing function, mutable-capture variables are stack-allocated (alloca) and their addresses are stored in the env struct. The store-load eliminator must not forward the original alloca's stored value past the MAKE_CLOSURE boundary, since the lifted function reads through the pointer later.

The existing `EscapeEntry` tracking marks allocas as escaped when they appear in CALL arguments. Verify that allocas also get marked escaped when they appear in `MAKE_CLOSURE.captures[]`. The `analyze_array_param_modes` function already iterates `make_closure.captures[]` -- the store-load-elim escape tracking needs the same check.

**Expression Inlining (`iron_lir_compute_inline_eligible`):**
MAKE_CLOSURE is already excluded from inline expression folding (its emission requires multiple C statements for env malloc and field stores). No changes needed.

## Anti-Patterns

### Anti-Pattern 1: Void* for All Closure Values

**What people do:** Store every closure as `void*` pointing to the function address. This is what the current code does: `void* _vN = (void*)__lambda_0;`

**Why it's wrong:** It loses the env pointer entirely. When a capturing closure is called through the `void*`, the env never reaches the lifted function. The current code produces wrong output for every non-trivial capture example (examples 2 through 20 all fail).

**Do this instead:** Store closures as `Iron_Closure` fat pointer structs `{ void *env; void (*fn)(void *); }`. Use a single generic struct for all closure types and cast the fn pointer to the correct signature at each call site.

### Anti-Pattern 2: Collecting Captures During HIR Lowering

**What people do:** Walk the lambda body inside `lower_lift_pending_hir` (Pass 3), find variable references that are not lambda parameters, and collect them as captures on the fly.

**Why it's wrong:** At that point in Pass 3, the scope stack from the enclosing function is gone -- the lambda body was lowered in Pass 2 using that scope, and Pass 3 runs after all Pass 2 scopes are popped. This is exactly why the current implementation cannot resolve what variables were captured. There is no scope context available in Pass 3 to distinguish "local to the lambda" from "outer variable."

**Do this instead:** Run capture analysis as a dedicated semantic pass before any HIR lowering, while the full scope chain is still intact in the AST. Annotate the `Iron_LambdaExpr` node directly. HIR lowering then reads the pre-computed annotation without needing scope context.

### Anti-Pattern 3: Env Struct Typedef Inside a Function Body

**What people do:** Emit the env struct typedef as a local declaration inside the lifted function body in C.

**Why it's wrong:** C11 does not allow `typedef struct { ... } name_t;` inside a function body. The struct definition must be at file scope.

**Do this instead:** Emit all env struct typedefs in a dedicated `env_typedefs` StrBuf section that is flushed before `lifted_funcs` in the final C output. Add `env_typedefs` as a field to `EmitCtx`.

### Anti-Pattern 4: Independent Env Copies for Shared Mutable Captures

**What people do:** Allocate a separate env struct per closure and copy the current value of the shared variable into each env at construction time.

**Why it's wrong:** Each closure gets its own copy of the variable at time of creation. Mutations by one closure won't be visible to the other. Example 11 (`inc` and `get` sharing `count`) would produce 0 instead of 3. Example 20 (game loop with multiple event closures) would be similarly broken.

**Do this instead:** The capture analysis pass detects when the same mutable variable is captured by two or more sibling closures. HIR lowering then allocates the shared variable as a heap cell (one malloc per shared variable, not one per closure). Each closure's env stores a pointer to the shared heap cell.

### Anti-Pattern 5: Inlining Lifted Closure Functions

**What people do:** Allow the function inlining pass to inline `__lambda_N` functions into their call sites like any other small function.

**Why it's wrong:** The inliner remaps parameter IDs assuming standard calling convention. It does not account for the `void *_env` first parameter or the typed cast that immediately follows inside the body. Inlining would clone `e->field` dereferences into the call site without the corresponding env setup, producing invalid C.

**Do this instead:** Guard the inliner to skip any function whose name starts with `__lambda_`, `__spawn_`, or `__pfor_`. These are intentionally boundary functions -- they are not candidates for inlining.

### Anti-Pattern 6: Using Generic _capN Field Names in Env Structs

**What people do:** Emit env struct fields as positional names `_cap0`, `_cap1`, `_cap2` in the order captures were collected.

**Why it's wrong:** It loses type information at the C level. The current MAKE_CLOSURE emit code already uses `_cap0`, `_cap1` style. This makes the generated C harder to read and debug, and breaks the typed-field requirement for mutable captures where `*e->count` vs `e->name` have different access semantics.

**Do this instead:** Use the original Iron variable name as the field name. The capture analysis pass records the name; the emit code uses it directly. For mutable captures, the field type is `T *fieldname` (pointer). For immutable captures, the field type is `T fieldname` (value). Named fields make the generated C self-documenting.

## Build Order

The following order respects all inter-stage dependencies:

```
Phase 1: Capture analysis pass (new)
    . capture_analysis.h + capture_analysis.c
    . Iron_CaptureVar struct in ast.h
    . Integration into analyzer.c pipeline after iron_typecheck()
    . Tests: basic free-variable detection for examples 1 and 2

Phase 2: HIR data model extension
    . Add captures[] + capture_count to IronHIR_Expr.closure in hir.h
    . Extend iron_hir_expr_closure() constructor in hir.c
    . Copy captures from AST annotation in hir_lower.c Pass 2 lower_expr_hir

Phase 3: Lifted function env param (HIR lowering Pass 3)
    . lower_lift_pending_hir: add void *_env first param to lifted func
    . Rewrite captured var refs in lifted body as env->field accesses
    . Tests: examples 1 (val capture), 2 (var capture), 5 (nested)

Phase 4: HIR -> LIR capture passing
    . CLOSURE case in hir_to_lir.c: populate captures[] with real LIR ValueIds
    . Tests: LIR dump shows MAKE_CLOSURE with correct capture operands

Phase 5: C emission -- env structs and fat pointers
    . env_typedefs StrBuf in EmitCtx
    . Typed env struct emission (named fields, pointer vs value per capture kind)
    . IRON_TYPE_FUNC -> Iron_Closure struct forward declaration
    . CALL indirect: f.fn(f.env, args...) pattern
    . Iron_Closure forward declaration in forward_decls section
    . Tests: examples 1-5 produce correct C output and correct program output

Phase 6: Shared captures
    . Shared variable detection in capture_analysis.c
    . HIR lowering: heap cell allocation for shared mutable variables
    . Tests: examples 11 (shared counter), 20 (game state)

Phase 7: Escaping closures (heap-lifetime env)
    . escape.c extension: detect closures returned from functions
    . Env malloc in enclosing function; no auto-free; lifetime owned by caller
    . Tests: examples 10 (make_counter), 18 (compose with three env levels)

Phase 8: Concurrency captures (spawn and parallel-for)
    . concurrency.c extension: validate spawn captures are read-only
    . Pass captured arrays to spawn/pfor env structs
    . Tests: examples 15 (spawn read-only array), 16 (parallel-for arrays)
```

## Sources

All findings are HIGH confidence -- derived from direct audit of production source files, not from training data or web search:

- `/Users/victor/code/iron-lang/src/hir/hir.h` — HIR data structures (IronHIR_Expr.closure has no captures[] field)
- `/Users/victor/code/iron-lang/src/lir/lir.h` — LIR data structures (IRON_LIR_MAKE_CLOSURE has captures[] but always populated with NULL/0)
- `/Users/victor/code/iron-lang/src/hir/hir_lower.c` — HIR lowering (Pass 3 creates lifted functions without env param; LIFT_LAMBDA case lines 1375-1411)
- `/Users/victor/code/iron-lang/src/hir/hir_to_lir.c` — HIR->LIR (CLOSURE case emits iron_lir_make_closure with NULL captures)
- `/Users/victor/code/iron-lang/src/lir/lir_optimize.c` — Optimization passes (MAKE_CLOSURE pure; captures[] already in operand rewriter; function inlining at line 2791)
- `/Users/victor/code/iron-lang/src/lir/emit_c.c` — C emission (MAKE_CLOSURE emit at line 2095; IRON_TYPE_FUNC -> "void*" at line 225; indirect CALL at line 1480)
- `/Users/victor/code/iron-lang/src/analyzer/escape.c` — Escape analysis (no closure awareness; only tracks heap allocations)
- `/Users/victor/code/iron-lang/src/analyzer/concurrency.c` — Concurrency check (spawn capture validation noted as future work at line 11)
- `/Users/victor/code/iron-lang/docs/lambda-capture/` — Expected C output for all 20 examples (closure_t fat ptr pattern, typed env struct fields, shared heap cells)

---
*Architecture research for: Iron compiler v0.1.0-alpha lambda capture system*
*Researched: 2026-04-02*
