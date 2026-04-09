# Static Interface Dispatch with Automatic Collection Splitting

## Overview

This document specifies a language design that provides the full abstraction of interfaces (programming against shared behavior contracts) while eliminating virtual dispatch overhead. The compiler achieves this by requiring whole-program visibility over source code, automatically constructing tagged unions for interface types, splitting heterogeneous collections into type-homogeneous internal storage, and applying context-dependent field-level layout transformations (AoS/SoA) based on semantic analysis of access patterns.

The programmer writes polymorphic code against interfaces. The compiler emits monomorphic, data-oriented code with no vtables, no heap indirection, and no pointer chasing in the common case. Data layout is optimized automatically at both the type level and the field level.

---

## 1. Core Principles

1. **Interfaces are a compile-time abstraction.** They define behavioral contracts but produce no runtime representation (no vtables, no type metadata objects).
2. **All libraries are distributed as source.** The compiler always has full visibility of every type that implements a given interface. Pre-compiled binaries of libraries are not supported.
3. **Dispatch is resolved statically.** Where the concrete type is known, calls are direct. Where multiple types are possible, the compiler generates a match over a tag — never a vtable lookup.
4. **Collections of interface types are split by concrete type internally.** The programmer sees one collection; the compiler maintains N homogeneous arrays plus an optional ordering index.
5. **Field layout is context-dependent.** Within each typed array, the compiler selects between Array-of-Structs and Structure-of-Arrays layout based on how each operation accesses the data. The same collection may use different layouts at different points in the program.

---

## 2. Interface Declaration

An interface declares a set of method signatures and associated types. It carries no runtime representation.

```
interface Shape {
    fn area(self) -> f64
    fn perimeter(self) -> f64
}
```

Types implement interfaces by providing concrete method bodies:

```
struct Circle { radius: f64 }
struct Square { side: f64 }
struct Rectangle { width: f64, height: f64 }

impl Shape for Circle {
    fn area(self) -> f64 { 3.14159 * self.radius * self.radius }
    fn perimeter(self) -> f64 { 2.0 * 3.14159 * self.radius }
}

impl Shape for Square {
    fn area(self) -> f64 { self.side * self.side }
    fn perimeter(self) -> f64 { 4.0 * self.side }
}

impl Shape for Rectangle {
    fn area(self) -> f64 { self.width * self.height }
    fn perimeter(self) -> f64 { 2.0 * (self.width + self.height) }
}
```

---

## 3. Automatic Union Construction

When a value is typed as an interface rather than a concrete type, the compiler determines the full set of implementors through whole-program analysis and constructs an internal tagged union.

Given the declarations above, a variable of type `Shape` is internally represented as:

```
// Compiler-generated (not user-visible)
enum Shape_Union {
    Circle(Circle),
    Square(Square),
    Rectangle(Rectangle),
}
```

The tag is a compact integer discriminant. Dispatch on interface methods becomes a match on this tag:

```
// Compiler-generated dispatch for shape.area()
match shape.tag {
    0 => Circle::area(shape.data as Circle),
    1 => Square::area(shape.data as Square),
    2 => Rectangle::area(shape.data as Rectangle),
}
```

This is fully resolved at compile time. No function pointers, no indirection.

---

## 4. Collection Splitting

### 4.1 Internal Representation

A collection declared as holding an interface type:

```
let shapes: List<Shape> = ...
```

is internally represented as a **split collection**: one homogeneous array per concrete type, plus an optional ordering index.

```
// Compiler-generated internal layout
struct ShapeList {
    circles: Vec<Circle>,
    squares: Vec<Square>,
    rectangles: Vec<Rectangle>,
    order: Option<Vec<(TypeTag, usize)>>,  // present only if needed
}
```

### 4.2 Insertion

When an element is added to the collection, it is routed to the appropriate typed array. An entry is appended to the order array if ordering is required.

```
shapes.push(Circle { radius: 5.0 })
// Internally: circles.push(Circle { radius: 5.0 })
//             order.push((TypeTag::Circle, circles.len() - 1))
```

### 4.3 Unordered Iteration

When the compiler can prove that iteration order does not affect program behavior, it emits a sequential loop over each typed array:

```
// User writes:
for shape in shapes {
    total += shape.area()
}

// Compiler emits:
for c in &shapes.circles {
    total += Circle::area(c)    // tight loop, fully inlineable
}
for s in &shapes.squares {
    total += Square::area(s)
}
for r in &shapes.rectangles {
    total += Rectangle::area(r)
}
```

No tags, no branches inside the loops, full cache locality within each type.

### 4.4 Ordered Iteration

When insertion order must be preserved (the element's position relative to other elements of different types is observed), the compiler emits iteration over the order array:

```
// User writes:
for (i, shape) in shapes.enumerate() {
    print("{i}: {shape.area()}")
}

// Compiler emits:
for (i, (tag, idx)) in shapes.order.iter().enumerate() {
    let area = match tag {
        Circle    => Circle::area(&shapes.circles[idx]),
        Square    => Square::area(&shapes.squares[idx]),
        Rectangle => Rectangle::area(&shapes.rectangles[idx]),
    };
    print("{i}: {area}")
}
```

This involves cross-array lookups and is comparable in cost to vtable-based dispatch. It is used only when necessary.

### 4.5 Strategy Selection

The compiler performs static analysis to determine which iteration strategy is required:

| Usage Pattern                              | Strategy         | Order Array |
|--------------------------------------------|------------------|-------------|
| Aggregation (sum, count, min, max)         | Unordered split  | Omitted     |
| Map / filter / collect                     | Unordered split  | Omitted     |
| Iteration with side effects only on self   | Unordered split  | Omitted     |
| Index-based access (`shapes[i]`)           | Ordered          | Present     |
| Iteration where cross-type order is observed | Ordered        | Present     |
| Mixed usage                                | Ordered          | Present     |

The default is conservative: if the compiler cannot prove order-independence, it includes the order array. An explicit annotation may allow the programmer to assert unordered semantics:

```
let shapes: List<Shape, unordered> = ...
```

---

## 5. Memory Layout Optimizations

### 5.1 Common Field Factoring

If all implementors of an interface share fields of the same type and offset, the compiler may factor them into shared storage:

```
// All shapes have a color field
struct Circle    { color: Color, radius: f64 }
struct Square    { color: Color, side: f64 }
struct Rectangle { color: Color, width: f64, height: f64 }

// Compiler may produce:
struct ShapeList {
    colors: Vec<Color>,           // shared field, stored once
    circles: Vec<CircleData>,     // radius only
    squares: Vec<SquareData>,     // side only
    rectangles: Vec<RectData>,    // width, height only
    order: Option<Vec<(TypeTag, usize)>>,
}
```

This improves cache utilization when operations access only the shared field.

### 5.2 Small/Large Variant Split

If one implementor is significantly larger than the others, the compiler may store it indirectly to avoid excessive padding in fallback (non-split) contexts:

```
// Threshold determined by compiler heuristic
if sizeof(Variant) > INLINE_THRESHOLD {
    store as heap-allocated pointer in tagged union contexts
} else {
    store inline
}
```

This applies only to non-split contexts (e.g., a single variable of interface type, not a collection). Split collections already avoid the padding problem entirely.

---

## 6. Context-Dependent Field-Level Splitting (Intra-Type SoA)

The type-level splitting described in Section 4 produces homogeneous arrays per concrete type. A second transformation can be applied within each typed array: splitting the struct's fields into separate arrays (Structure of Arrays, or SoA). The compiler selects between Array-of-Structs (AoS) and Structure-of-Arrays (SoA) layout per type, per call site, based on semantic analysis of how the data is accessed.

### 6.1 The Two Layout Strategies

**Array of Structs (AoS)** — the default. Each object's fields are stored contiguously:

```
circles: [radius1, color1] [radius2, color2] [radius3, color3]
```

This layout is optimal when operations access most or all fields of a single object, or when individual objects are accessed by index.

**Structure of Arrays (SoA)** — fields are separated into parallel arrays:

```
circles.radius: [radius1, radius2, radius3]
circles.color:  [color1,  color2,  color3]
```

This layout is optimal when operations iterate over many objects but access only a subset of fields per iteration.

### 6.2 Compositional Splitting

The two levels of transformation compose. For a `List<Shape>` where each shape has a `color` and type-specific geometry fields, the fully split layout becomes:

```
// Level 1: split by type
// Level 2: split by field within each type

struct ShapeList {
    // Circle storage (SoA)
    circle_radius: Vec<f64>,
    circle_color:  Vec<Color>,

    // Square storage (SoA)
    square_side:  Vec<f64>,
    square_color: Vec<Color>,

    // Rectangle storage (SoA)
    rect_width:  Vec<f64>,
    rect_height: Vec<f64>,
    rect_color:  Vec<Color>,

    order: Option<Vec<(TypeTag, usize)>>,
}
```

When computing `shape.area()` across all shapes, the compiler emits loops that touch only the geometry arrays. The color arrays are never loaded into cache.

### 6.3 Semantic Context Analysis

The compiler determines the optimal layout by analyzing the access pattern at each usage site. The same collection may use different layouts at different points in the program. The compiler does not commit to a single global layout; instead, it may maintain multiple representations or convert between them when the cost is justified.

The analysis classifies each usage into one of the following contexts:

| Semantic Context | Layout | Rationale |
|---|---|---|
| **Bulk field iteration**: loop touches 1–2 fields across many objects | SoA | Only accessed fields enter cache; enables SIMD vectorization |
| **Bulk full iteration**: loop touches all fields across many objects | AoS | All data is used; SoA would scatter related fields across memory |
| **Single-object access**: read/write all fields of one object by index | AoS | One cache line loads the entire object |
| **Single-field lookup**: access one field of one object by index | SoA | Avoids loading unrelated fields |
| **Mixed access**: some operations are bulk, some are single-object | AoS (default) | Conservative; avoids conversion overhead |
| **Small collection** (fits in L1 cache) | AoS | Layout is irrelevant; AoS avoids complexity |

### 6.4 Conversion Between Layouts

When a collection is used in both bulk-field and single-object contexts within the same function or module, the compiler has two options:

**Option A: Maintain dual representations.** Store data in SoA form (the more restrictive layout) and reconstruct AoS views on demand for single-object access. This is beneficial when bulk iteration dominates and single-object access is rare.

**Option B: Use AoS and project on demand.** Store data in AoS form and extract field slices into temporary SoA buffers before hot loops. This is beneficial when single-object access dominates and bulk iteration is occasional.

The compiler selects between these based on the ratio of bulk-to-single access sites and the size of the collection. For collections below a configurable threshold (e.g., fits in L1 cache), conversion is never performed — AoS is used unconditionally.

### 6.5 Interaction with Unordered Iteration

SoA splitting composes especially well with the unordered iteration strategy from Section 4.3. When the compiler can prove order-independence and has determined SoA is beneficial, the emitted code achieves maximum throughput:

```
// User writes:
let total_area = shapes.map(|s| s.area()).sum()

// Compiler emits (unordered + SoA):
let mut total = 0.0;

// Circles: touch only radius array, SIMD-friendly
for i in 0..circle_radius.len() {
    total += 3.14159 * circle_radius[i] * circle_radius[i];
}

// Squares: touch only side array
for i in 0..square_side.len() {
    total += square_side[i] * square_side[i];
}

// Rectangles: touch width and height arrays, interleaved
for i in 0..rect_width.len() {
    total += rect_width[i] * rect_height[i];
}
```

Each inner loop is a tight, branchless, SIMD-vectorizable operation over contiguous memory with no unused data in cache. This is the optimal code a human would write given full knowledge of the data — generated automatically from a polymorphic one-liner.

### 6.6 Programmer Hints

While the compiler performs layout selection automatically, the programmer may override the decision with annotations when they have domain knowledge the compiler cannot infer:

```
// Force SoA for a specific collection
let particles: List<Particle, layout: soa> = ...

// Force AoS for a specific collection
let entities: List<Entity, layout: aos> = ...

// Let the compiler decide (default)
let shapes: List<Shape> = ...
```

These annotations are hints, not directives. The compiler may ignore them if its analysis determines they would degrade performance (e.g., forcing SoA on a collection that is only ever accessed by index). A compiler warning is emitted in such cases.

---

## 7. Compilation Model

### 7.1 Source-Only Distribution

All dependencies must be available as source code at compile time. The compiler performs whole-program analysis to determine the complete set of implementors for each interface. There is no stable ABI for interface types.

### 7.2 Incremental Compilation

Adding a new implementor of an interface triggers recompilation of:

- The union type for that interface
- All collections holding that interface type
- All functions that dispatch on that interface

The compiler should support incremental compilation to minimize the impact. Unchanged concrete types and their associated code paths are not recompiled.

### 7.3 Dead Implementor Elimination

If an implementor is never instantiated, the compiler excludes it from the generated union and all associated dispatch branches. This keeps the union minimal.

---

## 8. Additional Optimizations and C Code Generation

The closed-world model, split-by-type storage, and SoA field splitting create a foundation that unlocks several further optimizations. Because this design targets C as its output language (rather than assembly or machine code), each optimization is described alongside its C code generation strategy. The generated C is designed to fall into the patterns that mature C compilers (GCC, Clang) optimize most aggressively — tight loops over contiguous arrays — effectively delegating low-level concerns like register allocation, instruction selection, and auto-vectorization to the backend.

### 8.1 Loop Fusion

When multiple collection operations are chained — map, filter, reduce, etc. — a naive implementation emits one loop per operation. The compiler can fuse them into a single pass per typed array.

```
// User writes:
let big_areas = shapes
    .map(|s| s.area())
    .filter(|a| a > 10.0)
    .sum()

// Without fusion (3 loops per type):
for i in 0..n { temp1[i] = circle_radius[i] * circle_radius[i] * PI; }
for i in 0..n { if temp1[i] > 10.0 { temp2.push(temp1[i]); } }
for i in 0..n { total += temp2[i]; }

// With fusion (1 loop per type):
for i in 0..n {
    double a = circle_radius[i] * circle_radius[i] * PI;
    if (a > 10.0) total += a;
}
```

**C generation:** This is purely a code structure decision. The compiler emits a single fused loop body instead of separate loops. No special C features are required. The fused loop also eliminates temporary allocations.

### 8.2 Dead Field Elimination

If a field of a concrete type is never accessed through any interface path in the entire program, the compiler can exclude it from the collection's storage entirely.

```
// Source types:
struct Circle { radius: f64, color: Color, debug_name: String }

// If debug_name is never accessed through any List<Shape> operation:
// Generated C struct for collection storage omits it:
typedef struct {
    double radius;
    Color color;
    // debug_name excluded — never accessed via interface
} Circle_CollectionData;
```

**C generation:** The compiler emits a reduced struct definition for collection storage. The full struct is used only in contexts where the concrete type is accessed directly. This is a compile-time decision with no runtime cost.

### 8.3 Monomorphic Collection Specialization

If dataflow analysis proves that a particular collection only ever contains one concrete type at a given usage site, the compiler eliminates the split structure, the tag, and the match entirely. The collection becomes a plain typed array with direct calls.

```
// User writes:
fn process(shapes: List<Shape>) { ... }

// If the compiler proves this function is only ever called
// with a list containing exclusively Circles:

// Generated C:
void process(double* circle_radius, size_t circle_count) {
    for (size_t i = 0; i < circle_count; i++) {
        // direct computation, no tag, no match, no split
        total += PI * circle_radius[i] * circle_radius[i];
    }
}
```

**C generation:** The compiler emits a specialized function signature and body. If the function is called from multiple sites with different type compositions, multiple specializations may be generated, similar to monomorphization. The original generic version is retained as a fallback.

### 8.4 Arena Allocation Per Type

Since the compiler manages the typed arrays internally and knows every type's size at compile time, it can allocate them from per-type arena pools. This eliminates per-object allocation overhead and makes bulk deallocation a single `free` call.

```
// Generated C:
typedef struct {
    double* radius;       // arena-allocated block
    Color*  color;        // arena-allocated block
    size_t  count;
    size_t  capacity;
} CircleStorage;

CircleStorage circle_storage_create(size_t initial_capacity) {
    CircleStorage s;
    s.radius = (double*)malloc(initial_capacity * sizeof(double));
    s.color  = (Color*)malloc(initial_capacity * sizeof(Color));
    s.count = 0;
    s.capacity = initial_capacity;
    return s;
}

void circle_storage_destroy(CircleStorage* s) {
    free(s->radius);
    free(s->color);
}
```

**C generation:** Standard `malloc`/`free`. Each field array in SoA mode gets one contiguous allocation. Growth uses `realloc` with geometric scaling. Destruction is one `free` per field array, regardless of element count.

### 8.5 Hardware Prefetch Insertion

With full knowledge of iteration patterns and memory layout, the compiler can insert prefetch hints to warm the cache ahead of the current iteration. This is particularly effective for SoA arrays where the access stride is known and uniform.

```
// Generated C:
#ifdef __GNUC__
  #define PREFETCH(addr) __builtin_prefetch(addr, 0, 3)
#elif defined(_MSC_VER)
  #include <xmmintrin.h>
  #define PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#else
  #define PREFETCH(addr) ((void)0)
#endif

// In a hot loop:
for (size_t i = 0; i < circle_count; i++) {
    PREFETCH(&circle_radius[i + PREFETCH_DISTANCE]);
    total += PI * circle_radius[i] * circle_radius[i];
}
```

**C generation:** The compiler emits `__builtin_prefetch` calls (GCC/Clang) or `_mm_prefetch` (MSVC) behind a portability macro. The `PREFETCH_DISTANCE` is a tunable constant — typically 8–16 elements ahead, calibrated to the target cache line size and expected latency. On compilers that do not support prefetch intrinsics, the macro expands to a no-op and the code remains valid.

Prefetch insertion is most valuable for:
- SoA arrays where the compiler accesses one field array per loop (stride is `sizeof(field_type)`)
- Ordered iteration via the order index array, where access into the typed arrays is less predictable

For unordered SoA iteration, modern hardware prefetchers often detect the sequential pattern automatically, making explicit prefetch less critical. The compiler should apply prefetch conservatively — only when access is non-sequential or the array is large enough that the hardware prefetcher may not cover the full working set.

### 8.6 Automatic Parallelism

The typed sub-arrays are independent data structures. When the compiler can prove that an operation has no cross-element side effects (via purity analysis or the `unordered` annotation from Section 4.5), it can parallelize iteration.

```
// Generated C (OpenMP):
#pragma omp parallel sections
{
    #pragma omp section
    {
        for (size_t i = 0; i < circle_count; i++) {
            total_circles += PI * circle_radius[i] * circle_radius[i];
        }
    }
    #pragma omp section
    {
        for (size_t i = 0; i < square_count; i++) {
            total_squares += square_side[i] * square_side[i];
        }
    }
    #pragma omp section
    {
        for (size_t i = 0; i < rect_count; i++) {
            total_rects += rect_width[i] * rect_height[i];
        }
    }
}
double total = total_circles + total_squares + total_rects;
```

**C generation:** The compiler emits OpenMP pragmas on the generated loops. For large typed arrays, it can also split a single typed array across cores using `#pragma omp parallel for`. OpenMP is widely supported by GCC, Clang, and MSVC. If the target C compiler does not support OpenMP, the pragmas are ignored and the code compiles serially with no correctness impact.

Two levels of parallelism are available:
- **Inter-type parallelism:** process each typed array on a different thread (as shown above). Useful when the collection has several types with significant element counts.
- **Intra-type parallelism:** split one large typed array across threads. Useful when one type dominates the collection.

The compiler selects the strategy based on estimated element counts when available, and may emit both levels for deeply nested pipelines.

### 8.7 Value Range Compression

With whole-program visibility, the compiler can analyze the actual range of values assigned to a field and use a smaller representation in collection storage.

```
// Source type:
struct Particle { position: Vec3, kind: u64 }

// If the compiler can prove `kind` only holds values 0–7
// across all assignments in the program:

// Generated C:
typedef struct {
    Vec3* position;
    uint8_t* kind;    // compressed from u64 to u8
    size_t count;
} ParticleStorage;

// Access widens on read:
uint64_t k = (uint64_t)particle_kind[i];
```

**C generation:** The compiler emits a narrower type in the storage struct and inserts widening casts at read sites. This is safe only when the range is statically provable — if the compiler cannot establish a bound, the original type is used. An optional profiling mode can gather runtime ranges during a training run and feed them back as hints, but the compiler must never narrow a type based on profiling alone without a static proof or programmer assertion.

### 8.8 Composition of Optimizations

These optimizations compose with each other and with the earlier transforms. The fully optimized generated C for a bulk operation over a heterogeneous interface collection combines:

1. **Type splitting** (Section 4) — separate arrays per concrete type
2. **SoA field splitting** (Section 6) — separate arrays per field within each type
3. **Loop fusion** (8.1) — one pass through the data
4. **Dead field elimination** (8.2) — unused fields not stored
5. **Prefetch insertion** (8.5) — cache warming for non-sequential access
6. **Automatic parallelism** (8.6) — independent typed arrays on separate threads
7. **Auto-vectorization by the C compiler** — tight SoA loops are ideal candidates

The result is that a single polymorphic expression in the source language compiles through to multi-threaded, vectorized, cache-optimal, zero-allocation C code operating on contiguous field arrays. The programmer writes at the level of interfaces and collections; the generated C is what a performance engineer would write by hand given full knowledge of the data layout and access patterns.

### 8.9 Reliance on the C Compiler Backend

By targeting C, this design deliberately delegates several concerns to the backend compiler:

| Concern | Handled by this compiler | Delegated to C compiler |
|---|---|---|
| Type-level collection splitting | Yes | — |
| Field-level AoS/SoA layout | Yes | — |
| Loop fusion | Yes | — |
| Prefetch insertion | Yes (via intrinsics) | — |
| Parallelism | Yes (via OpenMP pragmas) | Pragma interpretation |
| Register allocation | — | Yes |
| Instruction selection | — | Yes |
| SIMD auto-vectorization | — | Yes |
| Instruction scheduling | — | Yes |
| Branch prediction hints | — | Yes (from code structure) |

This is a deliberate design choice. The high-level structural optimizations — splitting, layout selection, fusion — are where this compiler adds value. The low-level machine-specific optimizations are where GCC and Clang excel. The generated C is *shaped* to be easy for the backend to optimize: simple loops, contiguous memory, no aliasing ambiguity, no indirect calls. The two compilers work in complementary layers.

---

## 9. Limitations

### 9.1 No Runtime Extensibility

Types that do not exist in the source at compile time cannot implement interfaces. This precludes:

- Runtime plugin loading via shared libraries
- Foreign function interface (FFI) based extensibility
- Object construction driven by external data where the type set is open-ended

### 9.2 Compile Time Scaling

Widely-implemented interfaces (e.g., `Printable`, `Serializable`) may produce large unions and many dispatch branches. Mitigation strategies include dead implementor elimination and incremental compilation, but pathological cases will produce longer compile times than vtable-based languages.

### 9.3 Binary Size

Each dispatch site generates a branch per implementor. For interfaces with many implementors used in many locations, this can cause code size growth. The compiler should apply branch deduplication and outlining where beneficial.

---

## 10. Comparison with Existing Approaches

| Property                     | Vtable (C++, Java) | Monomorphization (Rust generics) | This Design              |
|------------------------------|---------------------|----------------------------------|--------------------------|
| Heterogeneous collections    | Yes                 | No                               | Yes                      |
| Inline storage               | No (heap + pointer) | Yes (single type)                | Yes (split arrays)       |
| Cache locality               | Poor                | Excellent (single type)          | Excellent (per-type)     |
| Field-level splitting (SoA)  | Manual              | Manual                           | Automatic, context-aware |
| Dispatch overhead            | Indirect call       | None (static)                    | Branch (tag match)       |
| Inlining possible            | Rarely              | Always                           | Always                   |
| Open type sets               | Yes                 | Yes                              | No (closed world)        |
| Separate compilation         | Yes                 | Partial                          | No (source required)     |
| Binary size                  | Small               | Large (per-type copies)          | Moderate (per-type branches) |

---

## 11. Prior Art and Related Work

This design draws on and unifies ideas from several distinct areas of language design, compiler optimization, and software architecture. No existing system combines all of these elements. This section maps the relationship between this design and the prior work it builds upon, and identifies where it diverges.

### 11.1 Virtual Dispatch and Devirtualization

The vtable mechanism for dynamic dispatch dates to early Simula and was formalized in C++. It remains the standard implementation for interfaces in C++, Java, C#, and most object-oriented languages. Its indirection cost is well-understood: one pointer chase to the vtable, a second to the function, with the resulting indirect call inhibiting inlining and branch prediction.

Modern compilers mitigate this through devirtualization — when the compiler can prove that only one or two concrete types reach a call site, it replaces the vtable lookup with a direct call or a branch. The JVM's HotSpot C2 compiler and GraalVM's JIT compiler both perform this as a speculative optimization guided by runtime profiling, with a deoptimization fallback if the speculation is violated. LLVM performs similar transformations at link time when whole-program information is available.

This design takes devirtualization to its logical conclusion: by guaranteeing whole-program visibility as a language requirement, the compiler can *always* devirtualize. Speculation and fallbacks are unnecessary because the set of implementors is known with certainty.

### 11.2 Tagged Unions and Sum Types

The use of tagged unions (also called sum types, discriminated unions, or algebraic data types) as an alternative to vtable-based polymorphism is well-established in the ML family (Standard ML, OCaml, Haskell) and has been adopted by Rust (`enum`), Swift (`enum` with associated values), TypeScript (discriminated unions), and C++ (`std::variant`).

In these languages, tagged unions are a *programmer-facing* construct — the developer explicitly defines the set of variants. This design automates the construction: the compiler builds the tagged union from the set of interface implementors, invisible to the programmer. The programmer writes against an open-looking interface; the compiler closes it.

### 11.3 Monomorphization

Rust and C++ templates both use monomorphization — generating specialized code per concrete type — to achieve zero-overhead generic programming. This eliminates dispatch entirely for statically-known types but cannot produce heterogeneous collections (a `Vec<T>` can only hold one `T`).

Rust addresses this gap with trait objects (`dyn Trait`), which reintroduce vtable-based dispatch. This design eliminates that gap: heterogeneous collections are supported without vtables by splitting storage per type and dispatching via tag match.

### 11.4 Entity Component Systems (ECS)

The game development community independently arrived at the split-by-type storage pattern through Entity Component Systems. Frameworks like Unity DOTS, Bevy (Rust), and EnTT (C++) store components in per-type contiguous arrays (archetypes), achieving excellent cache behavior for bulk iteration. Entities are indices that cross-reference into these arrays.

ECS is an *architectural pattern* that the programmer adopts manually. It requires restructuring program design around components rather than objects. This design achieves the same memory layout automatically — the programmer writes in terms of interfaces and objects, while the compiler produces ECS-like storage internally.

### 11.5 AoS-to-SoA Transformation

The distinction between Array-of-Structs and Structure-of-Arrays layouts has been studied extensively in high-performance computing, GPU programming, and database systems.

**Language-level support (programmer-directed).** JAI, the programming language under development by Jonathan Blow, introduced a native `SOA` keyword that transparently converts a struct's storage layout. The programmer adds the keyword; the compiler handles the rest. This is widely regarded as JAI's most distinctive contribution to language design. Zig provides `MultiArrayList` in its standard library, achieving SoA layout through compile-time type construction rather than a language keyword. Both require the programmer to opt in explicitly.

**Semi-automatic compiler transforms.** Radtke and Weinzierl (2024–2025) proposed a C++ language extension using attributes (annotations) that guide the Clang compiler to convert AoS data into SoA scratchpads before hot loops and synchronize results back afterward. Their key insight is the concept of a *view* — restricting the transformation to only the accessed fields of a struct. Their work demonstrated that temporary, local AoS-to-SoA reordering is affordable when scoped to relevant fields, challenging the assumption that conversion overhead dominates. However, the programmer must still place the annotations.

**LLVM-level implementations.** CompilerTree Technologies implemented structure peeling, splitting, field reordering, and struct-array copy as LLVM module passes. Pérard-Gayot et al. (2018) showed that a selective SoA transformation within LLVM enabled vectorized ray-tracing code to reach within 10% of hand-vectorized Intel libraries — whereas without the transformation, vectorization actually *degraded* performance below the scalar baseline.

**Library-level abstractions.** The C++ game development community has built template-based SoA containers where the abstraction compiles down to zero-overhead code. Strzodka (2012) formalized the ASA (Abstraction for AoS and SoA) approach in C++, providing a single `operator[]` syntax that works across both layouts.

**What is missing from all prior work** is fully automatic, annotation-free selection of AoS vs. SoA based on semantic analysis of access patterns. Every existing approach requires the programmer to either choose the layout (JAI, Zig, library containers) or mark where transformation should occur (Radtke & Weinzierl). No existing compiler analyzes how each usage site accesses a collection and selects the optimal layout per site without programmer guidance.

### 11.6 Whole-Program Compilation and Closed-World Assumptions

The requirement that all source code be visible at compile time is shared by several systems:

- **Go** compiles all packages from source and performs whole-program escape analysis.
- **Rust** compiles crates from source with aggressive monomorphization and link-time optimization.
- **GraalVM Native Image** operates under a closed-world assumption for ahead-of-time compilation, enabling optimizations that are impossible under Java's open-world model.
- **MLton** (Standard ML compiler) performs whole-program compilation, enabling aggressive optimizations including unboxing, flattening, and defunctionalization.

This design extends the closed-world approach further than any of these: it uses whole-program knowledge not merely to optimize, but to determine the fundamental representation of interface types and their collections.

### 11.7 Synthesis: What This Design Unifies

The following table maps each component of this design to its origin and identifies what is new:

| Component | Prior Art | What This Design Adds |
|---|---|---|
| Eliminating vtables | Devirtualization (HotSpot, GraalVM, LLVM) | Guaranteed, not speculative; a language property, not an optimization |
| Tagged unions for dispatch | Rust enums, C++ variant, ML sum types | Automatically constructed from interface implementors |
| Split-by-type storage | ECS (Unity DOTS, Bevy, EnTT) | Transparent to the programmer; generated from interface declarations |
| AoS/SoA field splitting | JAI SOA keyword, Zig MultiArrayList, Radtke & Weinzierl annotations | Fully automatic, context-dependent, per-usage-site, no annotations |
| Whole-program compilation | Go, Rust, GraalVM Native Image, MLton | Extended to drive type representation and data layout decisions |
| Order-independence analysis | Database query optimizers, functional purity analysis | Applied to collection iteration to eliminate ordering overhead |

The novelty is not in any single technique but in their *composition under a unified language contract*. The source-only distribution requirement (Section 7.1) enables the closed-world analysis. The closed-world analysis enables automatic union construction (Section 3). Automatic union construction enables type-level collection splitting (Section 4). Collection splitting produces homogeneous arrays that are amenable to field-level SoA transformation (Section 6). And semantic context analysis (Section 6.3) selects the optimal layout per usage site without programmer intervention.

Each decision flows from the previous one. Remove any link in the chain — allow pre-compiled binaries, permit open type sets, or require programmer annotation for layout — and the design collapses to something that already exists.

---

## 12. Summary

This design eliminates virtual dispatch by leveraging whole-program knowledge to statically resolve all interface operations. Collections of interface types are split into per-type homogeneous arrays, yielding data-oriented performance from object-oriented abstractions. Within each typed array, the compiler further applies field-level splitting (SoA) based on semantic context analysis, ensuring that only the data actually accessed by each operation enters the cache. Ordering is preserved through an optional index array that the compiler includes only when needed.

The design unifies techniques from compiler optimization (devirtualization, monomorphization), language design (tagged unions, sum types), software architecture (Entity Component Systems, data-oriented design), and high-performance computing (AoS/SoA transformation). While each technique exists independently in prior work, no existing language or compiler combines them into a single, transparent abstraction where the programmer writes polymorphic code against interfaces and the compiler automatically produces data-oriented, cache-optimal, SIMD-friendly machine code.

The tradeoff is explicit: runtime extensibility is sacrificed in exchange for the guarantee that the programmer never pays for abstraction. For the large class of programs where all types are known at compile time, this design provides interface-level ergonomics with hand-optimized, data-oriented performance — automatically.
