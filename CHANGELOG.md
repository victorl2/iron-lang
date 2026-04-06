# Changelog

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
