# Pitfalls Research

**Domain:** Adding lambda capture (all 20 canonical patterns) to an existing C-targeting compiler with lambda lifting, ref counting, spawn/await concurrency, and an optimization pipeline
**Researched:** 2026-04-02
**Confidence:** HIGH (derived directly from Iron's source code: `src/lir/emit_c.c`, `src/lir/lir_optimize.c`, `src/hir/hir_to_lir.c`, `src/hir/hir_lower.c`, `src/lir/lir.h`, `src/hir/hir.h`, `src/runtime/iron_rc.c`, `src/runtime/iron_runtime.h`, and all 20 canonical examples in `docs/lambda-capture/`)

---

## Critical Pitfalls

### Pitfall 1: Capture Collection Not Wired — MAKE_CLOSURE Always Has Zero Captures

**What goes wrong:**
`hir_to_lir.c` line 882-883 calls `iron_lir_make_closure(... NULL, 0 ...)` unconditionally. The `captures` array and `capture_count` are hardcoded to empty regardless of which outer variables the lambda body references. Every lambda is generated as a non-capturing closure, which means all 20 examples fail at runtime because the lifted function body accesses undefined variables — the outer values were never packed into the env struct.

**Why it happens:**
Lambda lifting (in `hir_lower.c`) assigns a `lifted_name` and queues the body for top-level emission, but it does not compute which outer variables are free in the body. The HIR closure node (`expr->closure`) has no `captured_vars` field — the free-variable analysis was never implemented. `hir_to_lir.c` then has nothing to populate `captures` with, so it passes `NULL, 0`.

**How to avoid:**
Before or during HIR lowering, perform a free-variable analysis on the lambda body: walk the body AST/HIR and collect all variable references whose definition is outside the lambda's own parameter list and local scope. Store this set on the HIR closure node as a `captured_vars` stb_ds array. In `hir_to_lir.c`, iterate `captured_vars` to populate the `captures` array before calling `iron_lir_make_closure`. The analysis must handle nested lambdas (free variables of an inner lambda propagate as captures of the outer lambda).

**Warning signs:**
- Any test with a variable referenced inside a lambda body that is declared outside it produces wrong output or crashes
- Examples 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 all fail
- Generated C has `__lambda_0_fn()` accessing variables that don't exist in its parameter list

**Phase to address:** Phase 1 (free-variable analysis and capture collection). This is the foundational blocker — nothing else works without it.

---

### Pitfall 2: Env Struct Typedef Never Emitted — C Compile Error on Every Closure with Captures

**What goes wrong:**
`emit_c.c` line 2111 emits `__lambda_0_env *_env_5 = (__lambda_0_env *)malloc(sizeof(__lambda_0_env));` but the typedef `typedef struct { ... } __lambda_0_env;` is never written to `struct_bodies`, `forward_decls`, or `lifted_funcs`. Clang rejects the generated C with `unknown type name '__lambda_0_env'` the moment `capture_count > 0`.

**Why it happens:**
The `emit_type_decls` phase (called in the Phase 2 pass of `iron_lir_emit_c`) only emits Iron `object`, `interface`, and `enum` type declarations. It has no knowledge of closure environment structs, because env structs are not first-class `IronLIR_TypeDecl` entries in the module — they are synthesized implicitly by `emit_instr` when it encounters a `MAKE_CLOSURE`. The typedef is referenced before it is defined.

**How to avoid:**
Two viable approaches. First (cleanest): emit the env struct typedef into `ctx->lifted_funcs` before the lifted function body is emitted. Since `lifted_funcs` is assembled before `implementations`, the typedef will appear before any code that uses it. The typedef must list one `_capN` field per captured value with its correct C type (not `void*`). Second: register each env struct as an `IronLIR_TypeDecl` during the `MAKE_CLOSURE` lowering pass so `emit_type_decls` handles it uniformly alongside other struct types.

**Warning signs:**
- Clang error `unknown type name '__lambda_0_env'` on any Iron source that creates a capturing lambda
- The generated `.c` file contains `malloc(sizeof(__lambda_0_env))` with no prior typedef
- This triggers on examples 2, 3, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 (all capturing lambdas)

**Phase to address:** Phase 1 (env struct typedef emission). Must be fixed before any end-to-end test can run.

---

### Pitfall 3: Env Pointer Disconnected from Closure Value — Calling Convention Broken

**What goes wrong:**
`emit_c.c` line 2122-2124 emits `void* _v5 = (void*)__lambda_0;` — the closure value stored in `_v5` is only the function pointer, not a `{fn, env}` pair. When the closure is called at an indirect call site, the emitter casts the stored `void*` to a function pointer and calls it with the original arguments, but never passes `_env_5` as an additional argument. The lifted function `__lambda_0_fn(void* env, ...)` receives garbage or NULL for its env parameter.

**Why it happens:**
Iron's current closure representation stores a single `void*` in LIR (the function pointer cast to void), with the env struct allocated as a side effect at the `MAKE_CLOSURE` site. There is no struct type that bundles `fn + env` together in the LIR value. When `_v5` reaches an indirect CALL site, `emit_instr` for CALL only knows `_v5` is a function pointer — it has no mechanism to also pass `_env_5`.

**How to avoid:**
Introduce a `{void* fn; void* env;}` closure struct as the canonical representation for `IRON_TYPE_FUNC` values in generated C. `emit_type_to_c` currently returns `"void*"` for `IRON_TYPE_FUNC` (line 224-225). Change it to emit a typedef like `IronClosure` that is a struct containing `fn` and `env` pointers. `MAKE_CLOSURE` emission populates both fields. The indirect CALL site passes `closure_val.env` as the first argument when calling `closure_val.fn`. This is the standard two-word closure representation used in the canonical C examples in `docs/lambda-capture/10-escaping-scope.md`.

**Warning signs:**
- Captured lambda calls produce incorrect output but non-capturing lambdas work
- The env struct is allocated but the captured values are never read — segfault or garbage in lifted function
- Examples 2, 5, 6, 7, 8, 10, 11, 17, 18, 19, 20 produce wrong output

**Phase to address:** Phase 1 (closure struct representation). Directly tied to the env struct typedef fix — both must be addressed together.

---

### Pitfall 4: Escaping Closure Env Freed at Caller Return — Dangling Pointer in Escaping Closures

**What goes wrong:**
For closures that escape their defining scope (examples 5, 10, 11, 18, 19), the env struct must outlive the function that created it. The current emitter uses `malloc` for the env struct but never calls `free`. For stack-allocated env structs (a possible future optimization), the dangling pointer is a crash: `make_counter()` returns a closure whose env points into the stack frame of the now-returned `make_counter` call. Example 10 (`make_counter`) is the canonical test for this.

**Why it happens:**
Two opposite failure modes. If env is on the heap with `malloc` and never freed: memory leak. If someone "optimizes" to stack-allocate the env because the lambda appears non-escaping, but escape analysis was wrong: dangling pointer. The escape analysis for closures must be conservative: any closure returned from a function, stored in a data structure (examples 8, 17), or passed to a spawned thread (examples 15, 16) escapes.

**How to avoid:**
Always heap-allocate env structs via `malloc`. Do not apply stack promotion to env structs until a proven escape analysis is in place. For env lifetime management, use reference counting: the env struct should carry a ref count (or be managed by `Iron_Rc`) that is incremented when the closure is stored and decremented when it is destroyed. For the initial implementation, a simpler rule: always `malloc`, never `free` env from the creating scope — the caller of the lifted function is responsible for freeing after last use.

**Warning signs:**
- Example 10 (`make_counter`) outputs wrong counter values — env's `n` is garbage
- Example 11 (`make_pair`) — `inc()` and `get()` disagree on `count` value
- Address sanitizer reports use-after-return if env ever gets stack-allocated

**Phase to address:** Phase 1 (env allocation) and Phase 2 (env lifetime / ref counting integration). Stack promotion of env must be explicitly guarded against.

---

### Pitfall 5: Two Closures Sharing Mutable State — Each Gets Its Own Copy Instead of Shared Pointer

**What goes wrong:**
Examples 11 (`make_pair`, two closures sharing `count`) and 20 (game loop, multiple closures sharing `state`) require that multiple closures reference the same heap-allocated variable. The naive implementation gives each closure its own copy of the captured value in its independent env struct. After `inc()` runs three times, `get()` still reads 0 because its env has a separate copy.

**Why it happens:**
When two closures in the same scope both capture `var count`, the free-variable analysis assigns each closure its own `_cap0 = count_at_creation_time` field. This snapshot semantic is correct for value captures but wrong for mutable variable captures, which require reference semantics: the capture should be a pointer to the heap-allocated slot holding `count`, not a copy of its current value.

**How to avoid:**
Distinguish between mutable captures (capture by reference — store a pointer to the mutable slot) and immutable captures (capture by value — copy the current value). For `var` variables that are captured by any lambda in the same scope, promote them to heap-allocated boxes (`int64_t *count = malloc(sizeof(int64_t)); *count = 0;`) at the point of declaration. Every capture of `count` stores the box pointer, not the current value. Both `inc_env._cap0 = count_ptr` and `get_env._cap0 = count_ptr` point to the same box. Mutations go through the pointer. The canonical C in `docs/lambda-capture/11-shared-mutable-state.md` shows this pattern exactly.

**Warning signs:**
- Example 11: `get()` returns 0 after multiple `inc()` calls — confirms independent copies
- Example 20: closures that write to `state` do not see each other's mutations
- Example 2: `count` outside the closure does not reflect mutations from inside it (ref-capture not value-capture)

**Phase to address:** Phase 2 (mutable capture promotion / capture-by-reference for `var` variables). The distinction between `val` (value capture) and `var` (reference capture) is the most semantically critical decision in the entire capture system.

---

### Pitfall 6: Capture-in-Loop Variable Snapshot vs. Reference Confusion — Classic Closure-in-Loop Bug

**What goes wrong:**
Example 4 creates closures inside a `for` loop, each capturing `val captured = i`. If the compiler incorrectly captures the loop variable `i` by reference (as a mutable `var` capture), all five closures share the same pointer, and after the loop ends all return 4 (the final value of `i`). The expected output is 0 and 3 for indices 0 and 3.

**Why it happens:**
The loop variable `i` is a `val` in Iron's `for i in range(5)` construct — it is immutable per iteration. But if the free-variable analysis does not distinguish between `val captured = i` (a new immutable variable per iteration) and `i` itself, it may treat `i` as a shared mutable. Each lambda body captures `captured`, not `i`, and `captured` is a new binding per loop iteration, so it should receive value-capture semantics.

**How to avoid:**
Ensure loop iteration variables (`i` in `for i in range(n)`) receive `val` semantics (captured by value, not by reference). When the lambda body references `val captured` which is declared inside the loop body, the capture collector must recognize it as a new binding per iteration and emit a copy, not a pointer to a shared slot. The `val captured = i` assignment inside the loop body creates a fresh `val` each iteration — this must translate to a fresh capture slot per closure creation, not a shared pointer.

**Warning signs:**
- Example 4: `callbacks[0]()` returns 4 instead of 0 — all closures share the same counter
- Example 4: `callbacks[3]()` returns 4 instead of 3
- Any for-loop benchmark that collects lambdas produces identical results regardless of index

**Phase to address:** Phase 2 (val/var capture distinction, per-iteration binding semantics). Can be verified with example 4 as a regression test.

---

### Pitfall 7: Optimizer's Copy Propagation Replaces Env Slot With Direct Value — Breaks Mutable Captures

**What goes wrong:**
The copy propagation pass (`run_copy_prop` in `lir_optimize.c`) replaces a LOAD from an alloca with the last stored value if the store is the only write to that alloca. For a mutable captured variable like `count` that is box-promoted to a heap pointer, if copy propagation runs before the lambda is created, it may substitute the literal `0` for `count_ptr`'s value — removing the heap indirection and breaking the shared-mutation semantics.

More concretely: the alloca for `count` is stored once (`STORE alloca_count <- 0`) and then its address is taken for the env. If copy prop sees the STORE and replaces all LOADs of `alloca_count` with `0`, the env slot gets `0` (a literal integer) instead of `alloca_count_addr` (a pointer). Mutable updates through the closure then write to the wrong location or crash.

**Why it happens:**
Copy propagation is designed for SSA-style single-assignment variables. Heap-boxed mutable captures are a new pattern that requires the optimizer to treat "address-of a heap-promoted alloca" as an escape that prevents the alloca from being copy-propagated away. The existing escape analysis in `emit_c.c` (lines 2599-2603) checks `MAKE_CLOSURE` captures for escaped heap arrays but does not cover heap-boxed scalars used as mutable capture slots.

**How to avoid:**
Mark any alloca that is heap-promoted for mutable capture as non-copy-propagatable. The simplest approach: in the LIR lowering for heap-boxed captures, emit a `HEAP_ALLOC` instruction (not a plain `ALLOCA`) for the mutable slot. `HEAP_ALLOC` is already treated as side-effecting and excluded from copy propagation. The capture in the env struct then stores the `HEAP_ALLOC` result ID. Copy propagation never touches side-effecting instructions.

**Warning signs:**
- Example 2 (`count += 1` inside lambda): after optimization, `count` in the outer scope never changes — copy propagation eliminated the heap indirection
- Example 11: `get()` returns 0 after `inc()` calls under optimization — same cause
- Regression between unoptimized (correct) and optimized (wrong) builds

**Phase to address:** Phase 3 (optimizer integration). Add a test that verifies correctness with all optimization passes enabled, not just with `-O0`.

---

### Pitfall 8: DCE Removes Env Struct Allocation As Dead Code — Closure Crashes

**What goes wrong:**
The DCE pass treats `MAKE_CLOSURE` as pure (line 2783 in `lir_optimize.c`): `case IRON_LIR_MAKE_CLOSURE: return true;`. If the closure value `_v5` is never referenced after creation (e.g., it is stored into an alloca that DCE determines is dead), DCE will remove the `MAKE_CLOSURE` instruction. The env struct allocation (`malloc`) and all capture stores are also removed. Any later code that dereferences the env pointer (if it escaped via a different path) will segfault.

Additionally, if the closure is created but immediately stored into a collection via an indirect call (e.g., `actions.push(func() {...})`), DCE may not recognize that the push call keeps the closure alive, and if the push call itself is incorrectly classified as pure, the closure allocation disappears.

**Why it happens:**
DCE seeds liveness from side-effecting instructions. A `MAKE_CLOSURE` that only allocates memory (which DCE considers pure) and whose result ID is stored into an alloca that is itself dead would be removed. The `malloc` inside the MAKE_CLOSURE emission is a side effect at the C level but is invisible to the LIR DCE pass, which operates on LIR semantics where `MAKE_CLOSURE` is marked pure.

**How to avoid:**
Mark `MAKE_CLOSURE` as side-effecting (not pure) in `iron_lir_instr_is_pure`. This prevents DCE from ever removing a closure allocation. The performance cost is minimal — closures are rare compared to arithmetic. Alternatively, seed the live set with any `MAKE_CLOSURE` whose result reaches a collection push or a STORE into an escaping alloca.

**Warning signs:**
- Example 8 (storing lambdas in `actions` array): `log.length()` returns 0 — closures were DCE'd before the push
- Example 4 (closures in a loop): fewer closures than expected are called — some were eliminated
- With high optimization levels, capturing lambdas silently vanish

**Phase to address:** Phase 3 (optimizer integration). Change `iron_lir_instr_is_pure` for `MAKE_CLOSURE` to return `false`. Verify examples 4 and 8 pass under full optimization.

---

### Pitfall 9: Env Struct Typedef Ordering — Forward Declaration Missing When Env Is Used Before Definition

**What goes wrong:**
The `lifted_funcs` buffer holds both the env struct typedefs and the lifted function bodies. If the env struct typedef is emitted into `lifted_funcs` when `MAKE_CLOSURE` is processed in a caller function (which goes to `implementations`), but the lifted function body references the env struct in its prologue, the order in the output file is: `implementations` (where the typedef is emitted) after `lifted_funcs` (where it is referenced). The lifted function has a forward reference to an undefined type.

**Why it happens:**
`emit_c.c` line 3264-3266: `lifted_funcs` is concatenated before `implementations` in the final output. The MAKE_CLOSURE handler runs in `emit_func_body` for the caller function, which goes into `implementations`. So the env typedef is emitted into `implementations` while the lifted function (which needs the typedef) is in `lifted_funcs`. `lifted_funcs` is output first, meaning the typedef follows its first use.

**How to avoid:**
Emit env struct typedefs into `forward_decls` or `struct_bodies` — both of which appear before `lifted_funcs` in the output. The simplest fix: when a `MAKE_CLOSURE` with captures is encountered during any function emission (caller or lifted), append the env typedef to `ctx->struct_bodies`. Guard with a "already emitted" set keyed on `lifted_func_name` to avoid duplicate typedefs.

**Warning signs:**
- Clang error `use of undeclared identifier '__lambda_0_env'` inside the lifted function body
- The env typedef appears in the output file after the code that uses it
- Only triggers when capture_count > 0

**Phase to address:** Phase 1 (env struct typedef emission). The typedef must go to `struct_bodies` or `forward_decls`, not into the caller's function emission buffer.

---

### Pitfall 10: Self-Capture in Object Method — `self` Captured as Value Copies the Struct, Mutations Lost

**What goes wrong:**
Example 6 (`Counter.make_incrementer` returns a lambda that mutates `self.value`). If `self` is captured by value (copied into the env struct), mutations through the closure write to the env struct's private copy of `Counter`, not the original object. `c.value` stays 0 while the env's copy reaches 2.

**Why it happens:**
`self` in Iron is a reference to the receiver object. If the capture system treats `self` as a regular `val` capture (copy the current value), it copies the entire `Counter` struct into the env. The closure then mutates `env->self.value`, not the caller's `c.value`.

**How to avoid:**
Capture `self` as a pointer (`Counter*`), not a copy. When the free-variable analysis encounters `self`, treat it as a reference capture regardless of whether it was declared `val` or not — object references are always captured by pointer. The env struct field type becomes `Counter *self` and the capture assignment is `env->self = &c` (or the address of the receiver, depending on how objects are represented in LIR).

**Warning signs:**
- Example 6: `c.value` is 0 after two `inc()` calls, but no error is reported
- Example 20: game state fields do not change after event callbacks fire
- Example 17 (`Button.on_click` mutates `click_count`): `click_count` stays 0

**Phase to address:** Phase 2 (object/self capture semantics). Must be handled separately from scalar variable capture — objects are always captured by reference.

---

### Pitfall 11: Spawn/Thread Capture Passes Stack Pointer Across Thread Boundary — Race Condition or Dangling Pointer

**What goes wrong:**
Example 15 (`spawn pool { ... }` reads `data` from the outer scope). If the env struct containing the captured `data` pointer is stack-allocated (in the spawning thread's frame), the spawned thread may run after the spawning scope returns and its stack frame is invalidated. The `data` pointer in the env becomes dangling. Even if the current code always heap-allocates env, if a future optimization applies stack-promotion to closure envs, this becomes a crash.

More subtly: example 16 (`parallel for` with `weights` and `results`): if `weights` is a stack array that is promoted and then captured into the parallel-for env, and the spawned threads outlive the loop body, the workers read from freed stack memory.

**Why it happens:**
Thread lifetime is generally longer than the scope that spawns them. The existing `PARALLEL_FOR` infrastructure in `emit_c.c` (lines 2211-2266) allocates the context struct via stack (`chunk_env_t envs[2]` in the canonical example), which is safe only because the outer function waits for all threads before returning. But if a future optimization inlines the outer function body or moves the env allocation to a caller scope, this safety guarantee breaks.

**How to avoid:**
Always heap-allocate env structs for spawn/thread captures. Never apply stack promotion to env structs whose closures are passed to `IRON_LIR_SPAWN` or `IRON_LIR_PARALLEL_FOR`. The escape analysis that already tracks `MAKE_CLOSURE` captures escaping through `SPAWN` (lines 2599-2603 in `emit_c.c`) should also mark those envs as ineligible for stack promotion.

**Warning signs:**
- Example 15 or 16: nondeterministic output or crash under thread sanitizer
- Address sanitizer reports stack-use-after-return on env pointer in thread body
- Only triggers under high concurrency or when the spawning function is inlined

**Phase to address:** Phase 4 (concurrency capture). Explicitly forbid stack-promotion for any env passed to SPAWN or PARALLEL_FOR, regardless of other optimization decisions.

---

### Pitfall 12: Function Inlining Breaks Captured Env Pointer — Inlined Body Accesses Wrong Env

**What goes wrong:**
The function inlining pass (added in v0.0.7-alpha) may attempt to inline a lifted closure function (`__lambda_0_fn`) at its call site. The lifted function has `void* env` as its first parameter. When inlined, the env parameter's alloca is copy-propagated to the `_env_5` pointer from the MAKE_CLOSURE site. If the inlined body then accesses `env->_cap0`, it is reading from `_env_5->_cap0` — which is correct. But if copy propagation has already substituted `_env_5->_cap0` with the original captured value (eliminating the pointer indirection), mutations through the env no longer work.

Separately: the inlining decision function `should_inline_call` checks for direct calls via `func_decl`. Closure calls are indirect calls (via `func_ptr`), so they will not be inlined by the current heuristic. But if a future optimization converts an indirect closure call to a direct call after constant propagation resolves the function pointer, the inlining pass may then try to inline it.

**Why it happens:**
The inlining pass was designed for regular function calls, not for closure-calling-convention calls that carry an implicit env parameter. If the pass inlines a closure body without understanding the env parameter convention, it will either drop the env parameter (incorrect) or inline a direct copy of the body that does not see mutations made between closure creation and invocation.

**How to avoid:**
Exclude functions whose name starts with `__lambda_` from the inlining candidate list. The `is_lifted_func` predicate already exists (`emit_c.c` line 2367) — reuse it in the inlining pass. This prevents any closure body from being inlined until the closure convention is fully understood. Add a comment: "Do not inline lambda bodies — closure call convention requires env parameter handling that the inliner does not support."

**Warning signs:**
- Example 10 (`make_counter`): counter value is always 0 or 1 after inlining — env mutation is lost
- A lambda body appears inlined into a call site without the env pointer being passed
- The inlining pass's size threshold is hit by a lambda body with many captures

**Phase to address:** Phase 3 (optimizer integration). Add `__lambda_` exclusion to the inlining guard before enabling any capture tests.

---

### Pitfall 13: Recursive Lambda via Captured Variable Reference — Self-Reference Through Stale Void* Copy

**What goes wrong:**
Example 19 (`var factorial` captures itself for recursion). The lambda captures a pointer to the `factorial` variable. After reassignment, the recursive call must go through the updated `factorial` value. If the capture stores the current `void*` value of `factorial` at creation time (a snapshot), the recursive lambda sees the placeholder function, not itself, and returns 0 or crashes.

**Why it happens:**
The initial `factorial` assignment creates a placeholder closure. The second assignment creates the real closure, which captures `factorial`. If mutable variable capture stores a pointer to `factorial`'s alloca (reference semantics), the recursive call loads the current value through the pointer and sees the real function. If it stores a copy of `factorial`'s value at creation time (value semantics), it sees the placeholder and returns 0.

**How to avoid:**
`factorial` is declared `var`, so it must receive reference capture semantics (mutable slot / pointer to the alloca). The recursive lambda's env must store `var factorial`'s alloca address, not its current value. When the lambda body executes `factorial(n-1)`, it loads the current function value from the captured alloca — which by then points to the real lambda.

**Warning signs:**
- Example 19: `factorial(6)` returns 0 instead of 720 — placeholder was called
- Example 19: stack overflow — the real lambda somehow captured itself at an infinite recursion depth
- Any Iron program using a self-referential lambda through a `var` binding misbehaves

**Phase to address:** Phase 2 (mutable variable capture). Same fix as pitfall 5 (var promotion) applied to function-typed variables.

---

### Pitfall 14: Three-Level Env Indirection in Chained Higher-Order Functions — Type Erasure Causes Wrong Calling Convention

**What goes wrong:**
Example 18 (`compose`) produces a lambda that captures `f` and `g`, which are themselves lambdas with their own envs. The `compose_env` struct must store two `{fn, env}` pairs, not two bare `void*` function pointers. If `IRON_TYPE_FUNC` is emitted as `void*` (current behavior, line 224-225 in `emit_c.c`), the env struct for `compose` stores two `void*` values — it loses the env pointers for `f` and `g`. Calling `f(g(x))` inside the compose body crashes because `f`'s env is NULL.

**Why it happens:**
`emit_type_to_c` maps `IRON_TYPE_FUNC` to `"void*"`. This is too coarse for closure values that are themselves captures. A captured closure value needs to be represented as a struct `{void* fn; void* env;}` to preserve both the function pointer and the associated environment. Flattening to `void*` loses the env half.

**How to avoid:**
The `IronClosure` struct fix from pitfall 3 resolves this transitively. Once `IRON_TYPE_FUNC` emits as `IronClosure` (a `{fn, env}` struct), capturing a closure-typed variable stores the full struct in the env. The `compose_env._cap0` becomes of type `IronClosure`, and the body of the compose lambda calls `e->_cap0.fn(e->_cap0.env, ...)`.

**Warning signs:**
- Example 9 (`apply` captures `double`): `apply(21)` crashes or returns wrong value
- Example 18 (`compose`): result is wrong or segfault at the inner closure call
- Any higher-order function that passes closures as arguments loses the env pointer

**Phase to address:** Phase 1 (IronClosure struct representation). Fixing `emit_type_to_c` for `IRON_TYPE_FUNC` resolves both pitfall 3 and this one.

---

### Pitfall 15: Missing Forward Declaration for Lifted Lambda Function — Clang Error When Closure Is Called Before Lifted Body

**What goes wrong:**
The function prototype pass (`emit_func_signature` called for all functions in module, lines 3209-3213 of `emit_c.c`) emits prototypes for all non-extern functions. However, the `lifted_funcs` buffer — where lambda bodies are emitted — is assembled after the prototypes section in the output file ordering. If the env struct typedef is emitted inside `lifted_funcs`, any prototype that references the env struct type in its parameter list would create a circular dependency.

More concretely: the lifted function `__lambda_0_fn(void* env, int64_t x)` uses `void*` for its env parameter, which is fine for the prototype. But if the env struct typedef is emitted in `lifted_funcs`, and the lifted function body uses the env struct typedef for the cast `__lambda_0_env *e = (__lambda_0_env *)env`, and `lifted_funcs` comes before `implementations` but after `struct_bodies`, the ordering is: forward_decls → struct_bodies → lifted_funcs → implementations. The env typedef in `struct_bodies` will be visible to the lifted function body in `lifted_funcs`. This ordering is correct.

**Why it happens:**
This pitfall is a risk if env struct typedefs are mistakenly placed in `lifted_funcs` rather than `struct_bodies`. The current code does not emit them anywhere, so this is a latent ordering issue that will manifest if the fix is applied to the wrong buffer.

**How to avoid:**
Always emit env struct typedefs into `ctx->struct_bodies` — never into `ctx->lifted_funcs`. The output section ordering `struct_bodies → lifted_funcs` guarantees the typedef is visible when the lifted function body references it.

**Warning signs:**
- Clang error `unknown type name '__lambda_0_env'` inside the lifted function body even after adding the typedef
- The typedef appears in the output file after the lifted function body

**Phase to address:** Phase 1 (env struct ordering). Check that the typedef is appended to `struct_bodies` and verify the output section order: `forward_decls → struct_bodies → enum_defs → lifted_funcs → implementations`.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Capture all outer variables unconditionally (no minimization) | Avoids need for precise free-variable analysis | Larger env structs, more heap allocation, harder to optimize | Acceptable for Phase 1; add minimization in a follow-up pass |
| Always capture mutable vars by reference (even if not mutated outside the lambda) | No false sharing; correct for examples 2, 11, 20 | Redundant heap boxing for values that could have been copied | Acceptable for Phase 1; add purity analysis to downgrade to value capture in follow-up |
| Never free env structs (leak them) | No dangling pointer risk | Memory leak for programs with many closures | Acceptable for initial correctness milestone; ref-count env in a follow-up |
| Use void* for fn and env separately instead of IronClosure struct | Less refactoring of existing CALL emission | Indirect calls through closures cannot pass env; all 20 examples that use captures at call sites break | Never acceptable — the two-word representation is required for correctness |
| Skip optimizer pass on lambda-lifting functions | Prevents optimizer from breaking capture semantics | Missing optimization opportunities in closure-heavy code | Acceptable for Phase 1/2; integrate optimizer support in Phase 3 |
| Treat all captures as value captures initially | Simple implementation | Examples 2, 4, 5, 6, 11, 17, 19, 20 fail with wrong output | Never acceptable — var capture must be reference capture from day 1 |

---

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Copy propagation + mutable capture box | Copy prop substitutes the heap-boxed value directly, removing the pointer indirection | Emit mutable capture boxes as `IRON_LIR_HEAP_ALLOC` (side-effecting, not copy-propagatable) not as plain `ALLOCA` |
| DCE + MAKE_CLOSURE | `MAKE_CLOSURE` is marked pure; DCE removes unused closure allocations even when the env has already escaped | Mark `MAKE_CLOSURE` as side-effecting (`iron_lir_instr_is_pure` returns false for it) |
| Function inlining + lifted lambda bodies | Inliner may inline `__lambda_N_fn` at direct call sites without understanding the env parameter convention | Add `is_lifted_func(callee->name)` guard to the inlining decision — exclude all `__lambda_` and `__pfor_` functions from inlining |
| Escape analysis + env stack promotion | `escaped_heap_ids` tracks arrays; a future stack-promotion pass could incorrectly stack-promote env structs that escape via SPAWN or RETURN | Extend escape tracking to cover `MAKE_CLOSURE` env pointers; mark them as escaped if the closure reaches RETURN, STORE to non-local alloca, or SPAWN |
| Parallel-for env + range hoisting | Range bound hoisting may hoist a computation that involves a captured array's length, creating a new LIR value ID that is not in the PARALLEL_FOR captures list | After range hoisting, re-scan the hoisted value and add it to PARALLEL_FOR captures if it reads from any captured array |
| Store-load elimination + ref-captured variables | Store-load elimination sees a STORE to the mutable-capture alloca (the initial `0`) followed by a LOAD, and eliminates the LOAD — but the closure's env pointer stores the alloca address, not the LOAD result | Treat any alloca whose address is stored in an env struct (reachable from MAKE_CLOSURE.captures) as escaped; exclude it from store-load elimination |
| IronClosure struct + indirect call emission | Existing indirect call emission casts func_ptr to `(ret_type (*)())` and calls with original args; after IronClosure, func_ptr is `closure.fn` and first arg must be `closure.env` | In the CALL emitter for indirect calls, detect when func_ptr is an `IronClosure` type and prepend `closure.env` as the first argument to the call |

---

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Heap-allocating env for every lambda creation including short-lived non-escaping lambdas | Benchmark shows high `malloc/free` traffic for functional patterns (map, filter, compose chains) | After Phase 1 correctness, add escape analysis to stack-promote non-escaping envs (closures that don't outlive their creating scope) | Closure-intensive benchmarks; game loop callbacks allocated on every frame |
| Boxing every captured `var` as a heap pointer even when only one closure captures it | Extra indirection on every read/write of captured mutable variable | After Phase 2, add single-ownership analysis: if exactly one closure captures a `var` and the var is not used after closure creation, capture by value (copy) instead of by pointer | Patterns like example 2 (single closure mutates count) — one extra dereference per access |
| Storing full env pointer as `void*` in every closure, even for zero-capture lambdas | Zero-capture closures carry unnecessary `void* env = NULL` field; extra word in every closure value | Retain the existing zero-capture optimization (emit just the function pointer) | Only a problem if IronClosure struct is added uniformly — keep the null-env fast path |
| Deep indirection in chained higher-order functions | Example 18 compose: three levels of env pointer chasing per call; performance significantly worse than direct calls | Inline trivial compose chains after escape analysis; or emit specialized structs | Functional-style pipelines with 3+ composition levels |

---

## "Looks Done But Isn't" Checklist

- [ ] **Capture collection:** `iron_lir_make_closure` is called with a non-null captures array for every lambda that references outer variables — verify with `assert(capture_count > 0)` for any lambda body that mentions a non-local variable
- [ ] **Env struct typedef:** the generated `.c` file contains `typedef struct { ... } __lambda_0_env;` before the first function that references `__lambda_0_env` — grep the output file for uses-before-definition
- [ ] **Closure calling convention:** indirect call sites for closures pass `closure_val.env` as first argument — verify by running example 2 and checking that the `count` variable in the outer scope changes
- [ ] **Mutable capture as reference:** example 11 (`make_pair`) produces `3` — both `inc` and `get` share the same count box
- [ ] **Val capture as value copy:** example 4 (`callbacks[0]()`) returns `0` and `callbacks[3]()` returns `3` — each closure has an independent copy
- [ ] **Escaping closure lifetime:** example 10 (`make_counter`) increments correctly across three calls — env outlives `make_counter` return
- [ ] **Optimizer compatibility:** run all 20 examples with full optimization enabled (`-O2` via clang on generated C) — results must match unoptimized output
- [ ] **Thread capture safety:** example 15 and 16 produce correct output under thread sanitizer — no data races or use-after-free on env structs
- [ ] **Recursive lambda:** example 19 returns `720` for `factorial(6)` — self-reference through captured var works
- [ ] **IronClosure for higher-order:** example 18 returns `25` for `transform(5)` — three-level env indirection resolved correctly

---

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Capture collection missing (NULL captures) | HIGH | Implement free-variable walker over HIR expr/stmt nodes; populate HIR closure node's captured_vars; wire into hir_to_lir.c CLOSURE case |
| Env struct typedef missing | LOW | Add typedef emission to `struct_bodies` in the MAKE_CLOSURE handler; gate with "already emitted" set keyed on lifted_func_name |
| Closure calling convention broken (fn only, no env) | HIGH | Add `IronClosure` typedef to runtime header; change `emit_type_to_c` for IRON_TYPE_FUNC; update MAKE_CLOSURE emitter to fill both fn and env fields; update indirect CALL emitter to prepend env as first argument |
| Mutable var captured by value instead of reference | MEDIUM | Add mutable-var promotion to `LIR_HEAP_ALLOC` in closure lowering; route all captures of `var` through the heap pointer |
| DCE removes live closures | LOW | Change `iron_lir_instr_is_pure` to return `false` for `IRON_LIR_MAKE_CLOSURE` |
| Optimizer breaks capture semantics | MEDIUM | Emit mutable capture boxes as HEAP_ALLOC; extend escape tracking to cover env pointers; add `__lambda_` guard to inliner |
| Thread dangling env pointer | MEDIUM | Mark spawn/pfor env pointers as escaped; never stack-promote them |

---

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Capture collection not wired | Phase 1 — free-variable analysis | Example 1 (single val capture) reads and prints correctly |
| Env struct typedef missing | Phase 1 — env struct emission | Generated C compiles without unknown-type errors |
| Closure calling convention broken | Phase 1 — IronClosure struct | Example 2 (mutable capture) changes outer count |
| Env freed before closure (escaping closures) | Phase 1 — always malloc env | Example 10 (make_counter) increments across calls |
| Two closures share same mutable var | Phase 2 — var promotion to heap box | Example 11 (make_pair) returns 3 |
| Loop closure snapshot semantics | Phase 2 — val vs var capture distinction | Example 4 (callbacks[0]()==0, callbacks[3]()==3) |
| Optimizer's copy prop breaks mutable captures | Phase 3 — optimizer integration | Examples 2 and 11 correct under full optimization |
| DCE removes live closure allocations | Phase 3 — MAKE_CLOSURE is_pure=false | Example 8 (actions collection) runs all 3 callbacks |
| Env struct typedef ordering | Phase 1 — emit to struct_bodies | No clang forward-reference errors in generated C |
| Self-capture copies struct | Phase 2 — object reference capture | Example 6 (Counter.value==2), Example 20 (game loop) |
| Spawn/thread dangling env | Phase 4 — concurrency capture | Examples 15 and 16 pass under thread sanitizer |
| Inliner breaks captured env | Phase 3 — exclude __lambda_ from inlining | Example 10 correct with function inlining enabled |
| Recursive lambda via var capture | Phase 2 — mutable function-typed var capture | Example 19 returns 720 |
| Three-level env indirection (higher-order) | Phase 1 — IronClosure struct (same fix as calling convention) | Example 18 returns 25 |
| Env typedef ordering in output file | Phase 1 — struct_bodies target for typedefs | Grep generated C: typedef before first reference |

---

## Sources

- `src/lir/emit_c.c` — MAKE_CLOSURE emission (lines 2095-2126): env struct malloc, capture field stores, void* fn assignment; indirect CALL emission (lines 1465-1493); emit_type_to_c for IRON_TYPE_FUNC (line 224-225); output section ordering (lines 3248-3271)
- `src/lir/lir_optimize.c` — MAKE_CLOSURE purity classification (line 2783); DCE liveness seeding (lines 1166-1244); copy propagation; MAKE_CLOSURE in opt_collect_operands (lines 932-936); inlining pass (lines 2791+); MAKE_CLOSURE excluded from inline-eligible (lines 1576-1579)
- `src/hir/hir_to_lir.c` — CLOSURE case: captures hardcoded to NULL, 0 (lines 873-883); is_lifted guard (lines 1482-1486)
- `src/hir/hir_lower.c` — lambda lifting with pending_lifts queue (lines 955-998); no free-variable analysis performed
- `src/lir/lir.h` — IRON_LIR_MAKE_CLOSURE instruction definition (lines 240-245); IRON_LIR_FUNC_REF; IronClosure struct absent
- `src/hir/hir.h` — IRON_HIR_EXPR_CLOSURE node (lines 311-318); no captured_vars field
- `src/runtime/iron_runtime.h` and `iron_rc.c` — Iron_Rc reference counting API (retain, release, create, destructor)
- `docs/lambda-capture/` — all 20 canonical examples with expected C implementations; especially 10 (escaping scope), 11 (shared mutable state), 15 (spawn capture), 18 (chained higher-order), 19 (recursive lambda)
- Standard compiler engineering: "Closure conversion" and "lambda lifting" in Appel "Compiling with Continuations"; Kelsey & Clinger "Revised^5 Report on Scheme" (free variables, environment representation); Hanson "Lambda: The Ultimate Imperative" (shared mutable state through closures)

---
*Pitfalls research for: Iron v0.1.0-alpha lambda capture implementation*
*Researched: 2026-04-02*
