# Project Research Summary

**Project:** Iron compiler — lambda capture system
**Domain:** Compiler internals — closure representation for a C-targeting transpiler
**Researched:** 2026-04-02
**Confidence:** HIGH

## Executive Summary

Iron already has lambda parsing, lambda lifting, and non-capturing lambda invocation. The
entire capture pipeline is missing. `MAKE_CLOSURE` in `hir_to_lir.c` unconditionally passes
`NULL, 0` for captures regardless of what the lambda body references. No capture analysis
pass exists. Lifted functions have no `void *_env` parameter. Closure values are stored as
bare `void*` function pointers with no environment. The result: every one of the 20 canonical
lambda-capture examples fails at runtime, either producing wrong output or crashing.

The correct approach — confirmed by direct audit of all pipeline stages and all 20
documented C reference implementations in `docs/lambda-capture/` — is a five-stage
implementation: (1) new capture analysis semantic pass, (2) HIR data model extension to
carry capture sets, (3) lifted function env parameter wiring, (4) `IronClosure` fat-pointer
struct representation in C emission, and (5) optimizer guards. No new external dependencies
are needed. Every change is internal to the existing compiler source tree using C11 and the
vendored infrastructure already present.

The primary risk is implementation order: the capture analysis pass must run after
`iron_typecheck()` but before HIR lowering — by the time Pass 3 of HIR lowering runs, the
scope chain that identifies free variables is gone. Three optimizer interactions (copy
propagation, DCE, and function inlining) can each silently break correct mutable captures if
not addressed before enabling full optimization. Always-malloc for env structs is the correct
conservative strategy for v0.1.0; stack promotion is a future optimization.

## Key Findings

### Recommended Stack

No new external libraries are required. All implementation is in C11 within the existing
compiler source tree. The `stb_ds.h` dynamic array library (already vendored) handles
capture sets. The arena allocator (`src/util/arena.h`, already present) manages capture
analysis result lifetimes. `Iron_Rc` ref-counting must NOT be used for closure environments
— plain `malloc`/`free` matches the canonical C output examples exactly and avoids atomic
overhead on every closure operation.

**Core components (changes, not new dependencies):**
- `src/analyzer/capture_analysis.c` (NEW): free-variable walk over each lambda body AST; annotates `Iron_LambdaExpr.captures[]` before HIR lowering
- `src/hir/hir_lower.c` (EXTEND): Pass 3 adds `void *_env` first param to lifted functions; rewrites captured variable references to `env->field` accesses
- `src/hir/hir_to_lir.c` (EXTEND): `CLOSURE` case populates `captures[]` with real LIR ValueIds; `CALL` indirect path passes `closure.env` as first argument
- `src/lir/emit_c.c` (EXTEND): emits `__lambda_N_env_t` typedefs into `struct_bodies`; changes `IRON_TYPE_FUNC` emission from `void*` to `Iron_Closure {fn, env}` struct; updates `MAKE_CLOSURE` and indirect `CALL` handlers
- `src/lir/lir_optimize.c` (GUARD): excludes `__lambda_` functions from inlining; marks `MAKE_CLOSURE` as side-effecting (not pure) to prevent DCE removal

### Expected Features

All 20 examples in `docs/lambda-capture/` are correctness requirements for this milestone — they are not optional or differentiating. They are ordered by implementation layer dependency:

**Must have (v0.1.0 milestone — all 20 examples):**
- Free variable analysis pass + env struct emission — foundational; nothing else works without it (Group A: examples 1, 3, 7, 12, 13, 14)
- Mutable capture via heap pointer — `var` captures stored as `T*` in env; mutations propagate to outer scope (Group B: examples 2, 6, 17, 20)
- Escaping closure lifetime — always-malloc env; env outlives the creating function (Group C: examples 5, 9, 10, 18)
- Shared mutable state — two sibling closures pointing to one heap cell for a shared `var` (Group D: example 11)
- Closure-in-loop per-iteration snapshot — `val captured = i` creates a fresh env slot per iteration (Group E: examples 4, 8)
- Recursive lambda via captured `var` reference — two-step placeholder-then-overwrite init (Group F: example 19)
- Spawn and parallel-for capture — extend existing concurrency infrastructure to accept and pass env structs (Group G: examples 15, 16)

**Should have (v0.1.x — after all 20 pass):**
- Typed fat-pointer structs per signature — `closure_<sig>_t` instead of generic `Iron_Closure {void*, void*}` with casts
- Capture diagnostics — source-snippet error when a variable referenced in a closure body was not capturable
- Closure performance benchmark — microbenchmark comparing indirect call through fat pointer vs. direct function call

**Defer to v2+:**
- Stack env optimization — detect non-escaping closures and use stack allocation instead of malloc; correctness first
- Ref-counted environments — only if explicit demand arises from programs that return closures into multiple owners

### Architecture Approach

The implementation threads capture metadata top-down through the compiler pipeline:
capture analysis annotates the AST `Iron_LambdaExpr`, HIR lowering reads those annotations
to wire the env parameter and rewrite captured references, HIR-to-LIR lowering populates the
`MAKE_CLOSURE` instruction's `captures[]` field with real LIR ValueIds, and C emission
generates the env struct typedef and fat-pointer call sites. No new HIR or LIR instruction
opcodes are needed — the existing `IRON_LIR_MAKE_CLOSURE` and `IRON_LIR_CALL` instructions
have the right shape; they just need their data fields correctly populated.

**Major components:**
1. `capture_analysis.c` (NEW, in `src/analyzer/`) — walks each lambda body while scope chain is intact; classifies captures as value (`val`) or reference (`var`); detects escaping and shared captures; attaches `Iron_CaptureVar[]` to `Iron_LambdaExpr`
2. HIR lowering Pass 3 extension (`hir_lower.c`) — creates lifted functions with `void *_env` first parameter; rewrites captured variable references inside the body to `((env_t *)_env)->field` accesses
3. C emission extension (`emit_c.c`) — emits `__lambda_N_env_t` typedef into `struct_bodies` (before `lifted_funcs`); changes `IRON_TYPE_FUNC` to emit `Iron_Closure {void *env; void (*fn)(void*); }` struct; updates MAKE_CLOSURE to malloc and populate env; updates indirect CALL to use `closure.fn(closure.env, args...)`
4. Optimizer guards (`lir_optimize.c`) — excludes `__lambda_*`, `__spawn_*`, `__pfor_*` functions from inlining; marks `MAKE_CLOSURE` as non-pure to prevent DCE; extends escape tracking to cover allocas referenced in `captures[]`

### Critical Pitfalls

1. **Capture collection never wired** — `hir_to_lir.c` hardcodes `NULL, 0` for every `MAKE_CLOSURE`; implement the free-variable analysis pass first and wire it before attempting any other capture work; this is the root cause of all 20 example failures
2. **`void*` closure representation loses env at call sites** — changing `emit_type_to_c` for `IRON_TYPE_FUNC` from `"void*"` to `"Iron_Closure"` and updating both `MAKE_CLOSURE` emission and indirect `CALL` emission must happen together as one atomic change; doing only one half produces silent wrong output
3. **Env struct typedef in wrong output section** — typedefs must go to `ctx->struct_bodies`, not into `ctx->lifted_funcs` or the caller's implementation buffer; the output order `struct_bodies → lifted_funcs → implementations` only provides the typedef before the lifted function body if it lands in `struct_bodies`
4. **Copy propagation and DCE break mutable captures** — emit mutable capture boxes as `IRON_LIR_HEAP_ALLOC` (side-effecting, immune to copy-prop); mark `MAKE_CLOSURE` as side-effecting (not pure) so DCE never removes a closure allocation; verify all 20 examples with full optimization enabled, not just `-O0`
5. **Function inlining breaks lifted closure bodies** — add `strncmp(name, "__lambda_", 9)` guard to the inliner's candidate scan before any closure tests; the existing `is_lifted_func` predicate can be reused; this must be in place before Phase 3 testing

## Implications for Roadmap

Based on the dependency graph in FEATURES.md and the build order in ARCHITECTURE.md, the
natural phase structure is eight phases. The first five are sequential hard dependencies.
Phases 6-8 can begin after Phase 5 is validated but should not be parallelized because they
all touch `emit_c.c`.

### Phase 1: Capture Analysis Pass + IronClosure Foundation

**Rationale:** Nothing else in the milestone can be implemented or tested until free-variable analysis produces capture annotations and the fat-pointer struct representation is in place. These two concerns must land together: capture annotations without IronClosure produces a compiler that populates `captures[]` but still emits `void*` at call sites; IronClosure without capture annotations produces typed fat pointers for closures that have no env to pass.

**Delivers:** The compiler infrastructure that all subsequent phases extend. Non-capturing lambdas continue to work. Capturing lambdas produce generated C that compiles (no unknown-type errors) even if runtime output is not yet correct.

**Addresses:** Group A examples (1, 3, 12, 13), Group E example 4 (immutable captures only), pitfalls 1, 2, 3, 9, 14, 15

**Avoids:** Typedef ordering bug (put env typedefs in `struct_bodies`); void* representation loss (introduce `Iron_Closure` struct)

**Files:** `src/analyzer/capture_analysis.c` (new), `src/analyzer/capture_analysis.h` (new), `src/analyzer/analyzer.c` (2-line call insertion), `src/hir/hir.h` (add `captures[]` to closure union), `src/hir/hir.c`, `src/lir/emit_c.c` (env typedef + IronClosure type)

### Phase 2: Mutable Capture (var Promotion to Heap Pointer)

**Rationale:** Group B (mutable captures) is the second hard dependency layer. Once immutable value captures work, mutable capture requires promoting `var` declarations captured by any lambda to heap-allocated cells. This is also the prerequisite for shared-state closures (Group D) and recursive lambdas (Group F).

**Delivers:** Examples 2, 6, 17, 20 (mutable captures) and example 4's full snapshot semantics (the `val captured = i` per-iteration fresh binding). The `val` vs `var` capture distinction is implemented here.

**Addresses:** Group B examples, Group E example 4 (var/val correctness), pitfalls 5, 6, 10, 13

**Avoids:** Treating `self` as a value capture (always capture object references by pointer); loop variable snapshot confusion (val = fresh binding per iteration, var = pointer to promoted heap cell)

**Files:** `src/analyzer/capture_analysis.c` (extend with mutable classification), `src/hir/hir_lower.c` (heap-promote var captures; emit `HEAP_ALLOC` not `ALLOCA` for promoted vars), `src/lir/emit_c.c` (pointer fields in env struct)

### Phase 3: Optimizer Guards + Integration Testing

**Rationale:** Before implementing escaping closures or shared state (which add lifetime complexity), all existing examples (1-4, 6, 7, 12-14, 17, 20) must pass under full optimization. The three optimizer interactions (copy propagation, DCE, function inlining) can each silently break correct captures, so they must be addressed as a dedicated phase before any additional complexity is layered on.

**Delivers:** All previously-implemented examples pass with `-O2` optimization on the generated C. A regression suite is established.

**Addresses:** Pitfalls 7 (copy propagation + mutable capture), 8 (DCE removes live closures), 12 (inlining breaks env convention)

**Avoids:** Discovering optimizer breakage only after all 20 examples are implemented (much harder to debug at that point)

**Files:** `src/lir/lir_optimize.c` (MAKE_CLOSURE is_pure=false; `__lambda_` inlining guard; alloca-in-captures escape marking)

### Phase 4: Escaping Closures (Heap-Lifetime Environments)

**Rationale:** Group C examples require closures that outlive their creating function. The conservative always-malloc strategy (already implied by the current env emission path) is correct; the challenge is ensuring mutable captured `var` values are also heap-allocated and that the escape analysis marks returning closures correctly.

**Delivers:** Examples 5, 9, 10, 18 — closures returned from functions, nested closures, chained higher-order functions with three levels of env indirection.

**Addresses:** Group C examples, pitfall 4 (env freed before closure), pitfall 11 (spawn dangling pointer)

**Avoids:** Stack-promoting env structs for closures that escape via `return` or `store` to a non-local alloca

**Files:** `src/analyzer/escape.c` (extend to detect closures returned from functions), `src/lir/emit_c.c` (no auto-free for escaping envs), `src/hir/hir_lower.c` (escaping closure env lifetime tagging)

### Phase 5: Shared Mutable State (Two Closures, One Heap Cell)

**Rationale:** Group D is the highest-complexity correctness requirement. It requires the capture analysis pass to detect when two sibling lambdas in the same scope capture the same mutable variable, and emit a single heap cell shared between both env structs.

**Delivers:** Examples 11 and 20 — two closures sharing a `count` variable and game-loop multi-closure shared object state.

**Addresses:** Group D examples, pitfall 5 (independent copies for shared mutable captures)

**Avoids:** Each closure getting its own copy of the variable at creation time (snapshot semantics applied to var)

**Files:** `src/analyzer/capture_analysis.c` (shared variable detection; mark `Iron_CaptureVar.shared`), `src/hir/hir_lower.c` (emit one `HEAP_ALLOC` per shared var; route both envs to same cell)

### Phase 6: Recursive Lambda (Self-Referential var Capture)

**Rationale:** Group F requires a two-step initialization pattern (placeholder closure, then overwrite). This is a narrow extension of the mutable var capture work from Phase 2. It is sequenced after shared state only because it requires the full mutable var infrastructure to be stable.

**Delivers:** Example 19 — `var factorial` captures itself via pointer; recursive calls resolve through the updated closure pointer.

**Addresses:** Group F, pitfall 13 (stale void* copy for recursive lambda)

**Files:** `src/hir/hir_lower.c` (placeholder-then-overwrite two-step init pattern for function-typed var captures)

### Phase 7: Concurrency Captures (Spawn + Parallel-For)

**Rationale:** Group G extends the existing spawn and parallel-for infrastructure to carry captured outer variables in their env structs. This is sequenced last among correctness phases because it touches `IRON_LIR_SPAWN` and `IRON_LIR_PARALLEL_FOR` paths that have additional complexity (thread safety, env lifetime across thread boundaries).

**Delivers:** Examples 15 (spawn with read-only array capture) and 16 (parallel-for with captured arrays).

**Addresses:** Group G, pitfall 11 (spawn env dangling pointer)

**Avoids:** Stack-promoting env structs passed to SPAWN or PARALLEL_FOR; mutable captures across spawn boundaries without Mutex (concurrency checker extension)

**Files:** `src/hir/hir_to_lir.c` (populate SPAWN and PARALLEL_FOR captures), `src/analyzer/concurrency.c` (validate spawn captures using capture sets), `src/lir/emit_c.c` (spawn/pfor env struct emission)

### Phase 8: Post-Correctness Differentiators

**Rationale:** Once all 20 examples pass under full optimization, add typed fat-pointer structs per signature (instead of generic `Iron_Closure` with casts), capture diagnostics, and the closure performance benchmark.

**Delivers:** Better generated C type safety; developer-friendly error messages for capture issues; benchmark data for closure call overhead.

**Addresses:** Typed fat-pointer differentiator; capture diagnostics; benchmark from FEATURES.md

**Files:** `src/lir/emit_c.c` (per-signature closure struct emission), `src/analyzer/` (new diagnostic), `tests/benchmarks/`

### Phase Ordering Rationale

- Phases 1-5 are a strict dependency chain: capture analysis → mutable capture → optimizer guards → escaping lifetime → shared state. No phase can be safely implemented before the preceding one is stable.
- Phase 3 (optimizer guards) is inserted before escaping closures specifically because optimizer bugs are harder to isolate when escape analysis complexity is also in play.
- Phases 6 and 7 are relatively independent of each other after Phase 5 and could theoretically be parallelized, but both touch `emit_c.c` so sequencing avoids merge conflicts.
- Phase 8 is gated on all 20 examples passing; it must not be started earlier.

### Research Flags

Phases with well-documented patterns (skip `/gsd:research-phase`):
- **Phases 1-6:** Canonical C output for all 20 patterns is in `docs/lambda-capture/`; architecture is fully specified from direct source audit; implementation path is unambiguous
- **Phase 3:** Optimizer interactions are fully described with file/line references in PITFALLS.md; no ambiguity

Phases that may benefit from deeper research:
- **Phase 7 (concurrency):** The existing pfor and spawn infrastructure in `hir_to_lir.c` and `emit_c.c` has partial capture wiring; review the existing `IRON_LIR_SPAWN` and `IRON_LIR_PARALLEL_FOR` struct layouts in `lir.h` before planning — the capture arrays exist but are always zero-length; understanding the existing chunk function generation pattern is prerequisite before designing Phase 7 implementation

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All recommended changes derived from direct source code audit; no external dependencies; existing infrastructure is sufficient |
| Features | HIGH | All 20 canonical C output files exist in `docs/lambda-capture/`; compiler source confirmed non-capturing lambdas work; capturing lambdas explicitly unsupported in existing tests |
| Architecture | HIGH | Every pipeline stage audited with file and line references; data flow traced end-to-end; build order validated against scope availability constraints |
| Pitfalls | HIGH | All 15 pitfalls derived from direct source audit with specific file/line numbers; 3 optimizer interactions confirmed from `lir_optimize.c` source |

**Overall confidence:** HIGH

### Gaps to Address

- **Env lifetime for shared-state closures:** The canonical C for example 11 shows `free(count)` called after `free(inc_env)` and `free(get_env)`. The compiler must emit this ordering. The exact lowering point where this ordering is determined is not yet specified — needs design during Phase 5 planning.
- **Zero-capture closure uniformity:** When `IronClosure` is introduced as the uniform type for all `IRON_TYPE_FUNC` values, zero-capture closures should use `{ .env = NULL, .fn = fn }` for type uniformity. Verify that introducing `IronClosure` for zero-capture closures does not break the existing passing test suite before widening scope.
- **`self` capture representation in LIR:** Example 6 requires capturing `self` as a pointer. How `self` is represented in LIR (whether as a value or an address) affects whether the capture is `&self` or `self` — needs verification against `hir_lower.c`'s method lowering path before implementing Phase 2's object capture case.
- **Spawn env lifetime across thread join:** For `spawn pool { ... }` (example 15), the env is valid for the lifetime of the spawn pool. Whether the existing spawn infrastructure joins before returning (making heap env vs stack env equivalent for correctness) needs confirmation against `emit_c.c`'s spawn block emission before Phase 7.

## Sources

### Primary (HIGH confidence)
- `docs/lambda-capture/01-capture-single-immutable.md` through `20-game-loop-shared-state.md` — all 20 canonical C output patterns; authored alongside the compiler
- `src/lir/emit_c.c` — direct audit: MAKE_CLOSURE handler (line 2095), IRON_TYPE_FUNC emission (line 224), indirect CALL (line 1480), output section ordering (line 3264)
- `src/hir/hir_lower.c` — Pass 3 LIFT_LAMBDA case (lines 1375-1411); confirmed no env param, no captured-ref rewriting
- `src/hir/hir_to_lir.c` — CLOSURE case line 882: `iron_lir_make_closure(..., NULL, 0, ...)` hardcoded
- `src/lir/lir_optimize.c` — MAKE_CLOSURE pure classification (line 2783); function inlining pass (line 2791)
- `src/hir/hir.h`, `src/lir/lir.h` — data structure audit confirming `captures[]` exists in LIR but is never populated; HIR closure union has no captures field
- `src/analyzer/escape.c` — confirmed escape model provides foundation; no closure-escape detection yet
- `src/analyzer/concurrency.c` — spawn capture validation noted as future work (line 11)

### Secondary (MEDIUM confidence)
- CPython closure implementation (cell objects) — precedent for shared-mutable-state heap cells; pattern is stable across compiler literature
- Crafting Interpreters: Closures chapter — upvalue model, shared mutable state via single upvalue per slot
- Matt Might: Closure Conversion — flat vs. linked closures, free variable analysis, env parameter threading

---
*Research completed: 2026-04-02*
*Ready for roadmap: yes*
