# Feature Research

**Domain:** Lambda capture system for a compiled language targeting C
**Researched:** 2026-04-02
**Confidence:** HIGH (compiler source code read directly; behavior specs in docs/lambda-capture/; C reference implementations provided for all 20 examples)

---

## Overview

Iron already has lambda parsing, lifting, and non-capturing invocation. The
entire capture pipeline is missing: free variable analysis is absent, and
`MAKE_CLOSURE` in `hir_to_lir.c` unconditionally passes `NULL, 0` (no
captures). The LIR instruction and C emitter scaffolding for `MAKE_CLOSURE`
exists but is never populated. The type system has `IRON_TYPE_FUNC` but no
`closure_t` struct representation. All existing closure tests are explicitly
non-capturing.

All 20 examples in `docs/lambda-capture.md` represent real programs Iron
developers will write. They are categorized below by implementation complexity
and dependency order, not by raw user value — all 20 are correctness
requirements for this milestone.

---

## Feature Landscape

### Table Stakes (All 20 Are Required)

These are grouped by **implementation layer** — earlier groups must be complete
before later groups can be built.

#### Group A: Free Variable Analysis + Flat Environment Struct (Foundation)

Every other capture pattern depends on this. Without a working free variable
analysis pass and env struct emission, nothing else in this milestone compiles.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 1. Single immutable value capture | `val name` read inside closure body | LOW | Free var walk over closure body; env struct with one typed field; lifted function receives `void *_env` parameter; reads as `((env_t *)_env)->field` |
| 3. Multiple variables, different types | Int + Float + String in one env struct | LOW | Env struct with mixed-type fields; correct C type mapping; alignment non-issue since clang handles it |
| 12. Capture inside if branch | Closure created inside a conditional branch | LOW | Free var analysis must run over the closure body AST regardless of which branch contains it |
| 13. Capture inside match arm | Captured var written inside match arm inside closure | LOW | Three nesting levels all resolve to the same captured env slot |
| 7. Passing capturing lambda as callback | `apply_twice(func() { total += 10 })` — callee sees `func()` type, unaware of env | MEDIUM | Closure value must be a fat pointer `(fn, env)`; call site must unpack both; `func()` parameter type maps to a fat-pointer struct |
| 14. Filter/map with captured threshold | `filter(nums, func(n) { n > threshold })` | MEDIUM | Higher-order function parameter passing; closure threaded as fat-pointer argument through function call boundary |

#### Group B: Mutable Capture (Reference Semantics Through the Env)

Requires Group A. A captured `var` must be stored as a pointer-to-heap in the
env, not a copy. Mutations inside the closure must be visible at the call site.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 2. Capture and mutate a variable | `count += 1` inside closure; `count` visible outside | MEDIUM | Captured `var` is heap-allocated (`int64_t *`); env field holds the pointer; reads/writes dereference through it; mutations propagate to the outer scope |
| 6. Capture self (object mutation) | `self.value += 1` inside closure; original object changes | MEDIUM | `self` captured as a pointer to the object; field writes go through the pointer; object captured by reference not copy |
| 17. Closure as object field (callback) | `Button.on_click` holds a closure; mutates `click_count` | MEDIUM | Object fields typed as `func()` must store fat pointers; env lifetime tied to the object's lifetime |
| 20. Game loop shared state | Multiple closures mutate different fields of the same object | MEDIUM | Object captured as a pointer; all closures share the same pointer; the definitive game-dev integration test |

#### Group C: Escaping Closures (Heap-Allocated Environment Lifetime)

Requires Group B. The env must survive the function that created it. The
conservative, correct implementation is: always heap-allocate the env via
`malloc` (which `MAKE_CLOSURE` in `emit_c.c` already does). The challenge is
ensuring that mutable captured variables also live on the heap rather than the
stack.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 5. Nested lambda / make_adder | Inner lambda captures `x` from enclosing function; closure is returned and used after the function returns | HIGH | Escape detection; `x` must be heap-allocated when the closure escapes; env freed only when all closures referencing it are done |
| 10. Returning a lambda (make_counter) | Env for `n` must outlive `make_counter` entirely | HIGH | Definitive lifetime test; `n` must be heap-allocated; successive calls mutate the same heap cell; correct output: 1, 2, 3 |
| 9. Lambda capturing a lambda | Closure value (fat pointer) stored in another closure's env | HIGH | Env field type is a full `closure_t` struct (fn + env); inner closures must outlive the outer |
| 18. Chained higher-order (compose) | Three levels of env indirection: compose captures f and g, which each capture a scalar | HIGH | Env stores two full fat-pointer structs for f and g; deep env chain; three levels of captured state |

#### Group D: Shared Mutable State (Two Closures, One Variable)

Requires Group C. Two closures must share exactly one heap allocation for a
captured mutable variable.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 11. Two closures sharing count | `inc` and `get` both reference the same `count` variable | HIGH | `count` is heap-allocated once; both env structs hold an `int64_t*` pointing to the same cell; mutations through `inc` are visible through `get` |

#### Group E: Closure-in-Loop (Per-Iteration Snapshot)

Requires Group A. Each loop iteration creates a fresh closure with its own env.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 4. Capture loop variable (snapshot) | Five closures in an array; each must return its own `captured` value (0, 1, 2, 3, 4) | MEDIUM | Example already uses `val captured = i` (snapshot pattern); each iteration allocates a fresh env because closure creation happens inside the loop body; five distinct envs |
| 8. Storing capturing lambdas in a collection | Multiple closures all referencing the same `log` list pushed to `[func()]` | MEDIUM | Closures stored as fat-pointer values in an array; collection element type must accommodate fat pointers; all closures share the same `log` reference |

#### Group F: Recursive Lambda (Self-Referential Capture)

Requires Group B. The closure captures a mutable function-typed variable and
calls through it recursively.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 19. Recursive lambda via var reference | Lambda captures `var factorial` by pointer; recursive call resolves through the captured pointer | HIGH | Captured `var` is a fat-pointer variable; env holds a `closure_t*`; two-step init: placeholder closure first, then overwrite with the real closure that captures it |

#### Group G: Concurrency Captures (Spawn / Parallel-For)

Requires Group A. The existing spawn and parallel-for infrastructure passes no
env to the thread functions. Both must be extended to accept and pass a capture
env struct.

| Example | What It Tests | Complexity | Key Requirement |
|---------|--------------|------------|-----------------|
| 15. Spawn with read-only capture | Spawned task reads `data` array from outer scope | HIGH | Spawn env struct carries captured array reference (pointer + length); thread function receives `void *env`; no mutex needed for read-only access |
| 16. Parallel-for with captured arrays | Each iteration reads `weights[i]`, writes `results[i]` | HIGH | Pfor chunk function receives env pointer; both `weights` and `results` captured by reference in env; existing pfor infrastructure extended to populate and pass env |

---

### Differentiators (Competitive Advantage)

Not required for the 20 examples but would elevate the milestone.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Typed fat-pointer emission | `closure_<sig>_t` structs emitted per unique function signature instead of raw `void*` everywhere | MEDIUM | Eliminates the current `void*` cast for closure values; improves type safety in generated C; better `-Wall` compliance; implement after correctness is proven |
| Capture-related diagnostics | Clear error when a variable referenced inside a closure body is not in scope for capture; "did you mean to capture X?" suggestion | MEDIUM | Rust-style source-snippet errors; PROJECT.md explicitly lists this as a milestone goal |
| Closure performance benchmark | Measure closure call overhead vs. direct function call | LOW | PROJECT.md explicitly requires this; one microbenchmark comparing indirect call through fat pointer vs. direct function call |
| Shared env deduplication warning | Detect when programmer accidentally creates two independent copies of a variable that should be shared (e.g., two closures capturing the same `var` that are separately constructed) | HIGH | Required for correctness in example 11; the compiler must recognize when closures in the same scope share a mutable capture |

---

### Anti-Features (Commonly Requested, Often Problematic)

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| Automatic capture-everything | Simpler syntax — no explicit capture list needed | Silently captures large objects; invisible heap allocations; makes performance cliffs impossible to predict; captures things the user intended as local | Iron's model: variables in scope are captured when they appear in the closure body; this is already implicit and consistent with the examples |
| Move semantics for captures | Rust-style `move` keyword for thread-safe transfer | Adds ownership tracking incompatible with Iron's manual-memory philosophy; large scope increase; Iron explicitly rejects a borrow checker | Value-copy for scalar captures; pointer for heap objects; programmer is responsible for lifetime; this is what the C reference implementations already do |
| Borrow checker on captured references | Prevents use-after-free when closures escape | This is a full language feature, not a closure feature; incompatible with Iron's design; would make the compiler significantly more complex | Document the pattern: escaping closures heap-allocate their env; this is explicit in all 20 C reference implementations |
| Defunctionalization (Roc-style dispatch) | Eliminates function-pointer overhead; replaces indirect calls with switch-on-int | Requires monomorphization of all higher-order call sites; incompatible with dynamic callback storage in objects/arrays; huge scope increase | Fat-pointer closures (fn + env); proven pattern; matches the C reference implementations exactly; indirect call overhead is negligible at -O3 |
| CPS transform | Required for some async models | Massive compiler complexity; not needed for Iron's spawn/await model; changes the entire code generation strategy | Keep the existing spawn/await handle model; closures passed to spawn carry captured values in an env struct, same as regular closures |
| Ref-counted environments | Automatic env lifetime when closures are copied and returned | Adds refcount overhead to every closure operation; complicates the representation; needed only for shared escaping closures | Heap-allocate all envs; programmer manages lifetime via Iron's existing memory model; add refcounting only if explicit demand arises |

---

## Feature Dependencies

```
[Group A: Free Variable Analysis + Env Struct]
    └──required by──> [Group B: Mutable Capture]
    └──required by──> [Group E: Closure-in-Loop]
    └──required by──> [Group G: Concurrency Captures]

[Group B: Mutable Capture]
    └──required by──> [Group C: Escaping Closures]
    └──required by──> [Group D: Shared Mutable State]
    └──required by──> [Group F: Recursive Lambda]

[Group C: Escaping Closures]
    └──required by──> [Group D: Shared Mutable State]

[Existing: Spawn infrastructure (hir_to_lir.c IRON_LIR_SPAWN)]
    └──extended by──> [Group G example 15]

[Existing: Parallel-for infrastructure (hir_to_lir.c IRON_LIR_PARALLEL_FOR)]
    └──extended by──> [Group G example 16]

[Existing: Object/field system (SET_FIELD / GET_FIELD)]
    └──used by──> [Group B examples 6, 17, 20]

[Existing: Array/collection system]
    └──used by──> [Group E example 8, Group G examples 15, 16]
```

### Dependency Notes

- **Group A requires a new free variable analysis pass.** The `IRON_HIR_EXPR_CLOSURE`
  node has no `captures` field. The pass must walk the closure body, collect variables
  referenced but not declared inside, and populate either the HIR closure node or the
  `MAKE_CLOSURE` capture list at HIR-to-LIR lowering time. Currently `hir_to_lir.c`
  line 882 passes `NULL, 0` unconditionally.

- **Group A requires fat-pointer call sites.** The current indirect call path in
  `emit_c.c` (line 1465+) casts the closure value to a function pointer and calls it.
  It must instead unpack the fat pointer `(fn, env)` and pass `env` as the first
  argument to `fn`. This affects every indirect call site that invokes a lambda.

- **Group B requires heap cells for mutable captures.** When a `var` is captured
  by a closure, its storage must move from stack-alloca to a heap-allocated cell
  (`malloc(sizeof(T))`). All accesses to that variable — both inside and outside the
  closure — must go through the heap pointer. The env field holds the pointer, not
  the value.

- **Group C is safe to implement conservatively.** Heap-allocating all envs (which
  `MAKE_CLOSURE` in `emit_c.c` already does via `malloc`) is correct for all cases,
  including non-escaping closures. The conservative approach is: always heap-allocate.
  Stack allocation of non-escaping envs is an optimization for a later milestone.

- **Group D requires shared-env generation.** When two or more closures in the same
  scope capture the same mutable variable, the compiler must recognize this and share
  one heap allocation for that variable between both env structs. Both env structs
  hold the same `T*`, not independent copies. This is the highest-complexity
  correctness requirement in the milestone.

- **Group G requires spawn/pfor signature extension.** The lifted functions for
  `spawn` and `parallel for` currently receive no env parameter. They must accept
  `void *_env` as their first argument, and the call site must allocate, populate,
  and pass the env struct. The `IRON_LIR_SPAWN` and `IRON_LIR_PARALLEL_FOR` structs
  in `lir.h` already have `captures` arrays (both currently always zero-length).

---

## MVP Definition

### Launch With (v0.1.0-alpha — this milestone)

All 20 examples must compile and produce correct output.

- [ ] **Free variable analysis** — walk closure body AST; collect variables referenced but not declared inside; populate `MAKE_CLOSURE` capture list — foundation for everything
- [ ] **Typed env struct emission** — for each unique lifted function, emit a `<name>_env` typedef struct with correctly-typed fields (currently done but never populated); emit the struct typedef before the function forward declaration
- [ ] **Env parameter threading** — lifted lambda functions receive `void *_env` as first parameter; body reads captures as `((env_t *)_env)->field`
- [ ] **Immutable capture** — scalar `val` captures copy value into env at closure creation; reads inside closure go through env struct
- [ ] **Mutable capture via heap pointer** — captured `var` is heap-allocated; env field holds `T*`; reads/writes dereference through the pointer; mutations visible outside
- [ ] **Fat-pointer call sites** — indirect calls through a closure variable unpack `(fn, env)` and pass env as first argument to fn
- [ ] **Closure-in-loop** — per-iteration env allocation is automatic because env is created fresh at each closure-creation site inside the loop body
- [ ] **Escaping closure lifetime** — all envs are heap-allocated (already true for the env struct); mutable captures additionally heap-allocate the variable cell itself
- [ ] **Shared mutable state** — two closures capturing the same `var` in the same scope share one heap allocation; both env structs hold the same pointer
- [ ] **Recursive lambda** — captured `var factorial` is a fat-pointer variable; env holds a `closure_t*`; placeholder-then-overwrite init pattern
- [ ] **Spawn capture** — spawn lifted function accepts env struct pointer; spawn call site allocates and passes env
- [ ] **Parallel-for capture** — pfor chunk function accepts env pointer; pfor call site allocates and passes env

### Add After Validation (v0.1.x)

- [ ] **Typed fat-pointer structs** — replace `void*` closure representation with `closure_<sig>_t` structs; triggers only after all 20 examples pass
- [ ] **Capture diagnostics** — "variable X used in closure body was not captured" error with source snippet and suggestion
- [ ] **Closure performance benchmark** — microbenchmark: closure call overhead vs. direct function call; PROJECT.md explicitly requires this

### Future Consideration (v2+)

- [ ] **Stack env optimization** — detect non-escaping closures and allocate env on stack instead of heap; correctness first, performance second
- [ ] **Ref-counted environments** — automatic env lifetime for closures returned and stored in multiple places; needed only if explicit demand arises from real programs

---

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| Free variable analysis | HIGH | MEDIUM | P1 |
| Typed env struct emission (populated) | HIGH | LOW | P1 |
| Env parameter threading in lifted functions | HIGH | LOW | P1 |
| Immutable capture (scalar copy in env) | HIGH | LOW | P1 |
| Mutable capture (heap pointer in env) | HIGH | MEDIUM | P1 |
| Fat-pointer call sites | HIGH | MEDIUM | P1 |
| Closure-in-loop (per-iteration env) | HIGH | LOW | P1 |
| Escaping closure lifetime | HIGH | LOW | P1 — conservative: always heap-allocate env |
| Shared mutable state (shared heap cell) | HIGH | HIGH | P1 |
| Recursive lambda (captured fn-type var) | MEDIUM | HIGH | P1 |
| Spawn capture | HIGH | MEDIUM | P1 |
| Parallel-for capture | HIGH | MEDIUM | P1 |
| Typed fat-pointer structs | MEDIUM | MEDIUM | P2 |
| Capture diagnostics | HIGH | MEDIUM | P2 |
| Closure performance benchmark | MEDIUM | LOW | P2 |
| Stack env optimization | LOW | HIGH | P3 |
| Ref-counted environments | LOW | HIGH | P3 |

**Priority key:**
- P1: Must have for milestone launch (all 20 examples passing)
- P2: Should have; add when core is working
- P3: Future milestone

---

## Competitor / Reference Analysis

| Feature | Rust | Swift | Go |
|---------|------|-------|----|
| Value capture | Copy for `Copy` types; explicit `move` for ownership transfer | Copy for value types, implicit | Value types copied by default |
| Mutable capture | `FnMut` trait; borrow checker enforces exclusivity | Reference capture with `[weak self]` or `[unowned self]` | Captured variables are shared by reference in goroutines |
| Escaping closure | `move` + `Box<dyn FnMut()>` for heap allocation; `@escaping` not a concept | `@escaping` annotation required; ARC manages lifetime | Closures in goroutines can outlive caller; GC handles lifetime |
| Shared mutable state | `Arc<Mutex<T>>` required for multiple owners | `NSLock` or `DispatchQueue` | Race condition possible; `-race` detector catches it |
| Closure in collection | `Vec<Box<dyn Fn()>>` — heap, dynamic dispatch | `[() -> Void]` — fat pointers internally | `[]func()` — slice of fat pointers |
| Concurrent capture | Move semantics + `Send` bound required | `@Sendable` annotation | Programmer responsible |
| Iron approach | — | — | Fat pointer `(fn, env)`; manual memory; programmer responsible |

Iron's model is closest to Go's: closures capture by reference by default,
programmer is responsible for lifetime, no safety net. The C reference
implementations in `docs/lambda-capture/` are the authoritative specification
for what the generated C must look like for each of the 20 patterns.

---

## Sources

- Iron compiler source code: `src/lir/emit_c.c`, `src/hir/hir_to_lir.c`, `src/hir/hir.h`, `src/lir/lir.h`, `src/analyzer/types.h` — direct read, HIGH confidence
- `docs/lambda-capture.md` — 20 canonical examples with expected output and C reference implementations, HIGH confidence
- `docs/lambda-capture/` directory — per-example C reference implementations for all 20 patterns, HIGH confidence
- `tests/integration/lambda_capture.iron` — confirms non-capturing lambdas work; capturing lambdas explicitly noted as unsupported, HIGH confidence
- [Crafting Interpreters: Closures chapter](https://craftinginterpreters.com/closures.html) — upvalue model, shared mutable state via single upvalue per slot, escaping via heap migration, MEDIUM confidence
- [Matt Might: Closure Conversion](https://matt.might.net/articles/closure-conversion/) — flat vs. linked closures, free variable analysis, env parameter threading, MEDIUM confidence
- [How Roc Compiles Closures](https://www.rwx.com/blog/how-roc-compiles-closures) — defunctionalization alternative (explicitly an anti-feature for Iron), MEDIUM confidence

---
*Feature research for: Iron lambda capture system (v0.1.0-alpha milestone)*
*Researched: 2026-04-02*
