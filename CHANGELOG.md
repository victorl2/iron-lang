# Changelog

## v1.0.0-alpha (2026-04-09)

### Static Interface Dispatch
- `impl` keyword replaces `implements` (no backward compatibility)
- Whole-program interface implementor registry with canonical tag assignment
- Tagged union generation: tag enum + payload union + outer struct per interface type
- Dispatch functions per interface method (switch on tag) — no vtables, no function pointers
- Auto-wrapping of concrete types into tagged unions at assignment, call, and return sites
- Dead implementor elimination prunes unused types from unions

### Collection Splitting
- `Iron_SplitList_<Iface>` with per-type sub-arrays instead of heterogeneous arrays
- Per-type loops for cache locality; order index preserves insertion order
- Prefetch insertion (`__builtin_prefetch`) for split loops

### Closure Captures
- Mutable `var` capture by reference with cross-boundary visibility
- Closures returned from functions (heap-allocated environment)
- Closures as object fields, nested lambdas, recursive lambdas via `var` self-reference
- Shared mutable captures between sibling closures

### Collection Methods
- `map`, `filter`, `reduce`, `forEach`, `sum` with method chaining
- Array extension method syntax: `func [T].method[U](...)` with generic type parameters
- Cross-type generics: `.map[U](f: func(T) -> U) -> [U]`
- Methods work on interface-typed split collections via inline per-type dispatch

### Layout Optimizations
- Dead field elimination from collection storage structs (interprocedural analysis)
- SoA/AoS automatic per-loop selection via `IRON_SOA_THRESHOLD` (default 50%)
- Common field factoring across implementors into shared arrays
- Variant split for large variants (>2x smallest AND >64 bytes) via heap pointer
- Layout annotations: `[T, layout: soa]`, `[T, layout: aos]`, `[T, unordered]`

### Loop Fusion & Monomorphic Specialization
- `@fusible` annotation marks stdlib methods as fusion targets
- Def-use chain detection via LIR pre-scan with escape analysis
- `arr.map(f).filter(g).reduce(init, h)` compiles to a single loop per concrete type with no intermediate allocation
- Split-collection fusion (SoA-aware, per-field array access)
- Monomorphic collection collapse to plain `Iron_List_<Type>` with direct field access
- Specialization registry prevents duplicate function bodies
- `--warn-fusion-break` CLI flag

### Value Range Compression
- New `value_range.c` dataflow module tracking ranges through literals, arithmetic, conditionals, and PHI nodes
- Type ladder narrowing: int64 → int32 → int16 → int8/uint8 based on proven field ranges
- Widening reads and narrowing writes preserve program semantics
- `--report-compression` CLI flag

### Arena Allocation
- `Iron_Arena` extended with `tracked_ptrs` and `iron_arena_realloc_tracked()`
- 1.5x geometric growth for per-type sub-arrays (start cap 8)
- Single `_iron_sl_free_all` call replaces per-array frees in generated code

### Analysis Improvements
- Interprocedural monomorphic detection across function returns and parameters
- Heuristic-gated specialization for small functions (≤50 LIR instructions, 1–2 call sites)
- Call-site return range propagation through CALL instructions
- Conditional branch narrowing: `if x < 100` narrows ranges in true/false branches, AND chains accumulate
- One-level unrolling for recursive functions

### Emitter Refactoring
- `emit_c.c` (~7120 lines) decomposed into 5 focused modules: `emit_c.c`, `emit_helpers.c`, `emit_structs.c`, `emit_split.c`, `emit_fusion.c`
- Zero behavioral changes

### Build
- Sanitizers moved behind `IRON_ENABLE_SANITIZERS` CMake option (default OFF), fixing 20TB virtual address reservation from unconditional ASan on debug builds
- `stb_ds` hash map leaks at end of `iron_lir_emit_c` freed

### Test Suite
- 293 integration tests (up from ~230)
- 11 LIR unit tests, 5 HIR unit tests, 44/44 CTest targets pass
- Edge cases (empty, single element, single implementor, zero-field), stress (10K+ elements, 10 implementors, 5–6 op fusion chains), and composition tests (SoA+fusion, dead field+compression, arena+SoA+dead field)
- Generated C verification via pattern grep (`uint8_t`, `_iron_sl_realloc_tracked`, per-field array names)
- Benchmark speed thresholds raised 1.5x → 2.5x across 113 per-problem configs

## v0.0.8-alpha (2026-04-06)

### Algebraic Data Types
- Enum variants with typed payloads (`enum Shape { Circle(Float), Rect(Float, Float) }`)
- Exhaustive pattern matching with `->` arrow syntax and destructuring
- Generic enums with monomorphization (`Option[T]`, `Result[T, E]`)
- Recursive variant auto-boxing (`enum Expr { BinOp(Expr, Op, Expr) }`)
- Methods on enums with `match self` dispatch

### Lambda Capture
- Complete closure capture system with typed environment structs
- `val` captured by value, `var` captured by reference
- Capture support across spawn/parallel-for concurrency primitives
- Verbose capture analysis via `--verbose` flag

### Stdlib Expansion
- 19 String built-in methods (upper, lower, trim, split, replace, index_of, char_at, to_int, repeat, pad_left, etc.)
- Math: asin, acos, atan2, sign, seed, random_float, log, log2, exp, hypot
- IO: read_line, append_file, basename, dirname, join_path, extension, is_dir, read_lines
- Time: Timer with accumulator API (update, done, reset)
- Log: set_level, level constants

### Semantic Analysis
- Match exhaustiveness checking for both payload and plain enums
- Definite assignment analysis (use-before-init detection)
- Cast safety validation with narrowing warnings and overflow errors
- Array/slice bounds checking at compile time
- Concurrency safety: mutation detection in parallel/spawn blocks
- Generic constraint enforcement at instantiation sites
- LIR verifier hardening (PHI type consistency, call argument validation)
- 15 new diagnostic codes, ~100 new unit tests

### Test Suite
- 236 integration tests (up from 138)
- 76 type checker unit tests
- 44/44 CTest targets pass

## v0.0.1-alpha (2026-03-26)

First public alpha release of the Iron programming language.

### Language Features

- **Type system**: `Int`, `Float`, `Bool`, `String`, nullable types (`T?`), type inference
- **Objects**: struct-like objects with `val`/`var` fields, methods via `func Type.method()`
- **Inheritance**: `extends` for single inheritance, `implements` for interfaces
- **Enums**: with explicit ordinal values
- **Generics**: with monomorphization (zero-cost abstractions)
- **Pattern matching**: `match` expressions with exhaustiveness checking
- **Lambdas**: `func(x: Int) -> Int { x * 2 }`
- **Null safety**: non-nullable by default, compiler-enforced narrowing after null checks

### Memory Management

- Stack allocation by default
- `heap` keyword for explicit heap allocation with auto-free
- `rc` for reference-counted shared ownership (atomic)
- `defer` for deterministic cleanup
- `leak` for intentional permanent allocations
- Escape analysis to optimize heap allocations

### Concurrency

- `pool("name", n)` thread pools
- `spawn` / `await` for background tasks
- `channel[T](cap)` for message passing
- `parallel` for loops with work stealing

### Standard Library

- **Built-in**: `print`, `println`, `len`, `range`, `min`, `max`, `clamp`, `abs`, `assert`
- **Collections**: `List[T]`, `Map[K,V]`, `Set[T]`
- **math**: trig, sqrt, pow, lerp, random
- **io**: file read/write
- **time**: timestamps, timers, sleep
- **log**: leveled logging (info, warn, error, debug)

### Compiler

- Full pipeline: lexer, parser, semantic analysis, C code generation
- Rust-style error diagnostics with source snippets and color
- Compile-time evaluation (`comptime`)
- `iron build` / `run` / `check` / `fmt` / `test` CLI commands
- Compiles to C, then to native binary via clang/gcc
- `-O3` optimization for release builds

### Graphics & FFI

- Raylib bindings (`raylib.iron`)
- `draw {}` blocks for render passes
- C FFI via `extern func` declarations

### Platforms

- macOS (arm64, x86_64)
- Linux (x86_64)
- Windows (experimental)
