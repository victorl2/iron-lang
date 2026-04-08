# @fusible Annotation Specification

## 1. Overview

The `@fusible` annotation marks collection methods as eligible for loop fusion optimization. When the compiler detects a chain of calls to fusible methods where each method's result feeds directly into the next method's input, it composes their per-element bodies into a single fused loop with no intermediate allocations.

For example, `arr.map(f).filter(g).sum()` ordinarily produces three separate loops (map allocates a new array, filter allocates another, sum iterates the final array). With fusion, the compiler emits a single loop that applies the map transform, tests the filter predicate, and accumulates the sum -- all in one pass over the source array.

The `@fusible` annotation serves as a whitelist gate. Only methods annotated `@fusible` participate in fusion. The compiler does NOT parse Iron function bodies to determine fusibility -- it uses hardcoded knowledge of each annotated method's per-element semantics.

---

## 2. Syntax

`@fusible` appears on the line immediately before a `func` declaration:

```iron
@fusible
func [T].map[U](f: func(T) -> U) -> [U] {}
```

The annotation is valid only before `func` and method declarations. The lexer emits an `IRON_TOK_AT` token for the `@` character. The parser then expects the identifier `fusible` after `@`, followed by `func`. Diagnostics are emitted for:

- `@` followed by anything other than `fusible` -- error: "expected 'fusible' after '@'"
- `@fusible` not followed by `func` -- error: "expected 'func' after '@fusible'"

The `@fusible` annotation sets `is_fusible = true` on the resulting `Iron_FuncDecl` or `Iron_MethodDecl` AST node.

---

## 3. Safety Requirements

A method is safe to annotate `@fusible` if and only if:

1. **Single iteration:** It iterates over `self` (the collection) exactly once, processing each element independently.
2. **No inter-element dependencies:** It applies a per-element transformation, predicate, or accumulation with no dependencies between elements (element N does not affect the processing of element N+1 or any other element).
3. **No source mutation:** It does not modify the source collection. The input collection is consumed read-only.
4. **Predictable output:** It produces either a new collection (map, filter) or a scalar result (reduce, sum). `forEach` returns void (side-effect only).
5. **Expressible per-element operation:** The per-element operation is expressible as: `element-in -> (transform | predicate-test | accumulate) -> element-out or scalar-update`.

These requirements ensure that fusing multiple methods into a single loop produces identical results to executing them separately.

---

## 4. Initial Fusible Methods

The following five collection methods are annotated `@fusible` in `src/stdlib/list.iron`:

| Method | Category | Signature | Per-element operation | Fused behavior |
|--------|----------|-----------|----------------------|----------------|
| `map` | Transform | `func [T].map[U](f: func(T) -> U) -> [U]` | Apply `f(elem)` producing a new element | Inline transform, pass result to next stage |
| `filter` | Predicate | `func [T].filter(f: func(T) -> Bool) -> [T]` | Apply `f(elem)` producing a Bool | Conditional: if true, pass element to next stage; if false, `continue` |
| `reduce` | Accumulate | `func [T].reduce[U](init: U, f: func(U, T) -> U) -> U` | Apply `f(acc, elem)` producing new accumulator | Update accumulator inline; initialized once before the loop |
| `forEach` | Terminal | `func [T].forEach(f: func(T))` | Apply `f(elem)` for side effect | Execute body inline, no result propagation |
| `sum` | Terminal | `func [T].sum() -> T` | `acc += elem` | Accumulate with addition |

---

## 5. Fusion Chain Rules

### Chain Detection

A fusion chain is a sequence of two or more calls to fusible methods where each call's result is used **only** as the input to the next call in the sequence. The compiler performs def-use chain analysis in the LIR (Low-level IR) emission phase to detect these chains.

### Detection Algorithm

1. **Collect fusible calls:** Scan all CALL instructions in the function. A call is fusible if its target function name matches `Iron_List_*_{map,filter,reduce,forEach,sum}`.
2. **Build value-origin map:** Propagate results through STORE/LOAD chains (handling intermediate variable storage: `val temp = arr.map(f); temp.filter(g)` is still a chain).
3. **Link calls:** For each fusible call, check if its collection argument (first arg) traces back to another fusible call's result via the value-origin map.
4. **Escape check:** For each intermediate value in a candidate chain (all nodes except the terminal), check the use count. If `use_count > 1`, the intermediate result is used elsewhere and cannot be eliminated -- break the chain at that point.
5. **Discard trivial chains:** Chains with only one node are discarded (nothing to fuse).

### Chain Break Rules

- **Non-fusible method call:** If a fusible call's result feeds into a non-fusible call (any method not annotated `@fusible`), the chain breaks. Subchains on either side of the break are fused independently.
- **Intermediate escape:** If an intermediate result has `use_count > 1` (used by something other than the next chain step), the chain breaks at that point.
- **Terminal position:** Terminal operations (`reduce`, `sum`, `forEach`) must be the last operation in a chain. They consume the element stream without producing a collection.

### Valid Chain Positions

| Method | Valid positions | Notes |
|--------|----------------|-------|
| `map` | Any | Transforms elements; output feeds next stage |
| `filter` | Any | Filters elements; output feeds next stage |
| `reduce` | Terminal only | Produces scalar, not collection |
| `forEach` | Terminal only | Returns void |
| `sum` | Terminal only | Produces scalar |

A chain like `arr.map(f).reduce(init, g).filter(h)` is invalid because `reduce` produces a scalar, not a collection -- it cannot feed into `filter`. The compiler detects this naturally because the types would not match.

---

## 6. Per-Element Body Extraction

When emitting a fused loop, the compiler generates a single loop over the source collection and composes each chain step's per-element operation inline. The operations are hardcoded based on the method name:

| Method | Fused loop body | Variable flow |
|--------|----------------|---------------|
| `map(f)` | `elem = f(env, elem);` | The mapped value replaces `elem` for the next stage |
| `filter(f)` | `if (!f(env, elem)) continue;` | Element is skipped if predicate fails; otherwise `elem` is unchanged |
| `reduce(init, f)` | `acc = f(env, acc, elem);` | Accumulator is updated; initialized to `init` before the loop |
| `forEach(f)` | `f(env, elem);` | Body executed for side effects; no result propagation |
| `sum()` | `acc += elem;` | Accumulator incremented by element value |

Where `env` is the closure environment pointer (or NULL for non-capturing lambdas).

**Important:** The compiler does NOT parse the Iron function body to determine these operations. It uses hardcoded knowledge of each method's semantics. The `@fusible` annotation is a whitelist gate -- only annotated methods get this treatment. If a method is annotated `@fusible` but its semantics differ from the hardcoded behavior, the fused output will be incorrect. This is why `@fusible` is restricted to stdlib methods in the initial implementation.

### Example: Fused Loop for `arr.map(f).filter(g).sum()`

Non-fused (three separate loops):
```c
Iron_List_int64_t _tmp1 = Iron_List_int64_t_map(&arr, f_fn, f_env);
Iron_List_int64_t _tmp2 = Iron_List_int64_t_filter(&_tmp1, g_fn, g_env);
int64_t result = Iron_List_int64_t_sum(&_tmp2);
```

Fused (single loop):
```c
int64_t _acc = 0;
for (int64_t _i = 0; _i < arr.count; _i++) {
    int64_t _elem = arr.data[_i];
    _elem = f_fn(f_env, _elem);        /* map */
    if (!g_fn(g_env, _elem)) continue;  /* filter */
    _acc += _elem;                       /* sum */
}
int64_t result = _acc;
```

---

## 7. Split Collection Behavior

When a fused chain operates on an interface-typed split collection (e.g., `[Shape]` backed by separate per-type sub-arrays), the compiler emits one fused loop per concrete type:

```c
/* shapes.map(area).filter(pred).reduce(0.0, add) on split [Shape] */

/* Fused loop for Circle sub-array */
double _acc = 0.0;
for (int64_t _i = 0; _i < shapes._circle_count; _i++) {
    Iron_Circle _elem = shapes._circle[_i];
    Iron_Shape _tagged;
    _tagged._tag = 1;
    _tagged._circle = _elem;
    double _mapped = area_fn(area_env, _tagged);
    if (!pred_fn(pred_env, _mapped)) continue;
    _acc = add_fn(add_env, _acc, _mapped);
}

/* Fused loop for Square sub-array -- continues from same accumulator */
for (int64_t _i = 0; _i < shapes._square_count; _i++) {
    Iron_Square _elem = shapes._square[_i];
    Iron_Shape _tagged;
    _tagged._tag = 2;
    _tagged._square = _elem;
    double _mapped = area_fn(area_env, _tagged);
    if (!pred_fn(pred_env, _mapped)) continue;
    _acc = add_fn(add_env, _acc, _mapped);
}
double result = _acc;
```

### Accumulation Rules for Split Collections

- **reduce/sum terminals:** Accumulation is sequential. The first type's fused loop runs to completion, then the second type's loop continues from the same accumulator. This produces deterministic results that match the non-fused ordered iteration.
- **map/filter terminals producing a collection:** Each per-type fused loop pushes to its own sub-array in the result split collection, preserving the split layout.

---

## 8. SoA-Aware Fusion

When a split collection uses SoA (Structure-of-Arrays) layout (Phase 48), fused loops access per-field arrays directly instead of struct items:

```c
/* SoA-aware fusion: particles.map(update_pos).filter(in_bounds).sum_energy() */
/* Particle fields stored as: particle_x[], particle_y[], particle_vx[], particle_vy[] */

double _acc = 0.0;
for (int64_t _i = 0; _i < particles._particle_count; _i++) {
    /* Reconstruct struct from SoA fields (only if lambda expects full object) */
    Iron_Particle _elem;
    _elem.x = particles._particle_x[_i];
    _elem.y = particles._particle_y[_i];
    _elem.vx = particles._particle_vx[_i];
    _elem.vy = particles._particle_vy[_i];

    Iron_Physics _tagged;
    _tagged._tag = 1;
    _tagged._particle = _elem;

    Iron_Physics _mapped = update_pos_fn(update_pos_env, _tagged);
    if (!in_bounds_fn(in_bounds_env, _mapped)) continue;
    _acc += sum_energy_fn(sum_energy_env, _mapped);
}
```

When the lambda only accesses specific fields, the compiler can avoid full struct reconstruction and pass individual field values directly (future optimization).

---

## 9. Compiler Flag: --warn-fusion-break

The `--warn-fusion-break` flag enables diagnostic output at each point where a potential fusion chain is broken. This helps developers understand which operations prevent fusion and optimize their code accordingly.

### Usage

```
ironc build --warn-fusion-break myfile.iron -o myfile
```

### Diagnostic Output

The flag emits notes to stderr at each chain break point:

```
note: --warn-fusion-break: fusion chain broken: non-fusible call after .map() in function main
note: --warn-fusion-break: fusion chain broken: intermediate result of .map() used elsewhere in function main
```

### Break Point Categories

1. **Non-fusible call boundary:** A fusible method's result feeds into a non-fusible call. The chain is split into independent sub-chains.
2. **Intermediate escape:** An intermediate value has `use_count > 1` -- it is used by something other than the next chain step. The compiler cannot eliminate the intermediate allocation.

### When to Use

- During performance optimization to identify chains that could benefit from restructuring
- To verify that expected fusion chains are being detected
- To understand why a chain of collection operations is not being fused

By default, no fusion diagnostics are emitted. The flag must be explicitly passed.

---

## 10. Examples

### Example 1: Simple Chain Fusion

```iron
val arr = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
val result = arr.map(func(x: Int) -> Int { x * x })
                .filter(func(x: Int) -> Bool { x > 10 })
                .sum()
```

**Fused output:** Single loop that squares each element, skips those <= 10, and accumulates the sum:

```c
int64_t _acc = 0;
for (int64_t _i = 0; _i < arr_count; _i++) {
    int64_t _elem = arr_data[_i];
    _elem = _elem * _elem;              /* map: x * x */
    if (!(_elem > 10)) continue;         /* filter: x > 10 */
    _acc += _elem;                       /* sum */
}
```

### Example 2: Split Collection with Reduce

```iron
val shapes: [Shape] = [Circle(5.0), Square(3.0), Circle(2.0)]
val total = shapes.map(func(s: Shape) -> Float { s.area() })
                  .filter(func(a: Float) -> Bool { a > 10.0 })
                  .reduce(0.0, func(acc: Float, a: Float) -> Float { acc + a })
```

**Fused output:** Two fused loops (one per concrete type), sequential accumulation:

```c
double _acc = 0.0;
/* Circle sub-array */
for (int64_t _i = 0; _i < shapes._circle_count; _i++) {
    /* ... map + filter + reduce inline ... */
}
/* Square sub-array (continues from same _acc) */
for (int64_t _i = 0; _i < shapes._square_count; _i++) {
    /* ... map + filter + reduce inline ... */
}
double total = _acc;
```

### Example 3: Broken Chain with Sub-Chains

```iron
val arr = [1, 2, 3, 4, 5]
val mapped = arr.map(func(x: Int) -> Int { x * 2 })
val custom = someNonFusibleTransform(mapped)
val result = custom.filter(func(x: Int) -> Bool { x > 5 }).sum()
```

**Result:** Two independent operations:
- `arr.map(...)` executes as a single (non-fused) call since there is no subsequent fusible consumer
- `custom.filter(...).sum()` is a 2-node fusion chain (single fused loop)

With `--warn-fusion-break`, the compiler would note that the chain broke at `someNonFusibleTransform`.

---

## 11. Future Extensions

### User-Defined @fusible Methods

The current implementation restricts `@fusible` to the five stdlib collection methods listed above. A future extension could allow user-defined methods to be annotated `@fusible`, which would require:

1. **Iron function body analysis:** The compiler would need to verify that the method's body satisfies the safety requirements (single iteration, no inter-element dependencies, no source mutation).
2. **Per-element body extraction:** The compiler would need to extract the per-element operation from the method's implementation, rather than relying on hardcoded semantics.
3. **Verification pass:** A static analysis pass that proves the method's behavior matches one of the fusible categories (transform, predicate, accumulate, terminal).

This is deferred because it requires parsing and analyzing Iron function bodies at the LIR level, which is substantially more complex than the current name-matching approach.
