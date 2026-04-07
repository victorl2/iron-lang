# Phase 47: Collection Methods - Context

**Gathered:** 2026-04-07
**Status:** Ready for planning

<domain>
## Phase Boundary

Add `.map()`, `.filter()`, `.reduce()`, `.forEach()`, and `.sum()` on arrays with method syntax and lambda arguments. Methods chain (`arr.map(...).filter(...).sum()`). Methods work on interface-typed split collections with per-type dispatch. Phase 49 (Loop Fusion) is responsible for fusing chained operations into single-pass loops — this phase delivers correct, working collection methods with real Iron implementations.

</domain>

<decisions>
## Implementation Decisions

### Method resolution
- **Stdlib declarations with Iron bodies** — methods are declared as `func [T].map[U](...)` in Iron stdlib files with real loop implementations (not compiler intrinsics)
- This requires adding **generic extension method syntax**: `func [T].method_name(...)` where `self` is the array
- The compiler compiles these like any method — no special-case magic for map/filter/reduce
- Phase 49 must recognize these as fusion targets and optimize chained calls into single-pass loops. This is an explicit goal of Phase 49 with full test coverage.

### Return types
- `.map(func(T) -> U)` returns `[U]` — a plain Iron array. Type `U` is inferred from the lambda return type.
- `.filter(func(T) -> Bool)` returns `[T]` — same element type, fewer elements
- `.reduce(init: U, func(U, T) -> U)` returns `U` — accumulator type inferred from `init`
- `.forEach(func(T))` returns nothing (Void)
- `.sum()` returns `T` (only valid for `[Int]` and `[Float]`)
- **Two generic type params** for reduce: `func [T].reduce[U](init: U, f: func(U, T) -> U) -> U`
- All returns are plain arrays (eager, not lazy). Chaining allocates intermediate arrays. Fusion in Phase 49 eliminates these.

### Split collection dispatch
- `.map()` on a split collection (`[Shape]` backed by `Circle[]` + `Square[]`) runs **per-type loops** — preserves cache locality
- If the return type is also an interface type, the result **preserves the split layout** (stays a split collection)
- If the return type is concrete (e.g., `.map(func(s) -> Int { s.area() })`), the result is a plain `[Int]`
- `.filter()` on a split collection runs **per-type filter loops**, results combine into a new split collection
- `.forEach()` and `.reduce()` on split collections run per-type loops naturally (since the Iron body uses `for item in self`)

### Error semantics
- **All compile-time enforcement** — Iron uses monomorphization, so every generic instantiation resolves to concrete types
- `.sum()` on non-numeric array: compile error (`"sum() requires [Int] or [Float], got [String]"`)
- `.map(f)` where `f` returns wrong type: standard type mismatch error
- `.reduce(init, f)` where init type doesn't match lambda return: type error
- **Empty arrays**: natural behavior — loops don't execute, `.map([])` returns `[]`, `.sum([])` returns `0`, `.reduce(init, f)` on `[]` returns `init`

### Claude's Discretion
- Array concatenation implementation for building results (push vs concat)
- How generic extension methods are registered in the symbol table
- Exact mangling for `[T].map` in C output
- Optimizer interaction with collection method bodies

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Static dispatch spec
- `docs/static-interface-dispatch-spec.md` — Full specification, sections 4 (Collection Splitting) and 8.1 (Loop Fusion) define how collection operations interact with split storage

### Existing collection infrastructure
- `src/runtime/iron_runtime.h` lines 306-372 — IRON_LIST_DECL/IMPL macros, Iron_List_T struct layout
- `src/lir/emit_c.c` — Split collection struct generation (Iron_SplitList_*), per-type loop emission, prefetch insertion
- `src/analyzer/typecheck.c` — Method call resolution on object types, array literal type inference for interface types
- `src/hir/hir_to_lir.c` lines 1594-1677 — For-loop lowering to LIR blocks

### Closure infrastructure
- `src/analyzer/capture.c` — Capture analysis (val + var captures)
- `src/hir/hir_lower.c` lines 1652-1756 — Lambda lifting with uniform `fn(void *_env, ...)` calling convention
- `src/lir/emit_c.c` lines 2875-2953 — MAKE_CLOSURE env struct emission

### Parser
- `src/parser/parser.c` lines 825-875 — `expr.method(args)` parsed as IRON_NODE_METHOD_CALL

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **IRON_LIST_DECL/IMPL macros**: Generate typed list operations (create, push, get, set, pop, len, free). Collection methods can use push() internally.
- **Iron_Closure fat pointer**: `{ .fn, .env }` — lambda arguments to map/filter/reduce will be Iron_Closure values
- **Split collection infrastructure**: `Iron_SplitList_<Iface>` with per-type sub-arrays, push functions, order index — already working from v0.1-alpha
- **Method call parsing**: `expr.method(args)` already parses as IRON_NODE_METHOD_CALL — no parser changes needed for the call syntax

### Established Patterns
- **Method resolution on objects**: `func Circle.area()` mangled to `Iron_circle_area`. Extension methods on arrays need similar resolution.
- **Generic monomorphization**: Enums already do this (`Iron_Option_Int`). Array methods need same treatment: `[Int].map` → `Iron_List_int64_t_map_...`
- **For-loop compilation**: `for item in self` compiles to index-based iteration with pre_header/header/body/inc/exit blocks

### Integration Points
- **Type checker**: Needs to resolve `.map()` on array types → find the generic extension method, instantiate with concrete types
- **HIR lower**: Lambda arguments to collection methods go through standard closure lifting
- **Emit_c.c**: May need to emit IRON_LIST_DECL/IMPL for result array types if not already instantiated

</code_context>

<specifics>
## Specific Ideas

- Method syntax: `func [T].map[U](f: func(T) -> U) -> [U]` — generic extension method on array types
- Iron bodies: real for-loops that compile normally, not compiler intrinsics
- Phase 49 must demonstrate fusion of these methods with full test coverage — chained `map.filter.reduce` must compile to a single fused loop per type with no intermediate allocation

</specifics>

<deferred>
## Deferred Ideas

- Lazy iterators — would enable natural fusion without compiler magic, but adds a new type system concept. Could replace eager array returns in a future milestone.
- `.first()`, `.last()`, `.contains()`, `.indexOf()` — useful collection methods but not in Phase 47 scope
- `.sort()`, `.reverse()` — mutation methods, different category

</deferred>

---

*Phase: 47-collection-methods*
*Context gathered: 2026-04-07*
