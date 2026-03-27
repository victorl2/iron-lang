# Requirements: Iron

**Defined:** 2025-03-25
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v1 Requirements

Requirements for initial release. Each maps to roadmap phases.

### Lexer

- [x] **LEX-01**: Compiler tokenizes all Iron keywords, operators, literals, and delimiters
- [x] **LEX-02**: Every token carries source span (file, line, column) for diagnostics
- [x] **LEX-03**: Lexer reports errors for unterminated strings and invalid characters with location
- [x] **LEX-04**: Comments (-- to end of line) are recognized and skipped

### Parser

- [x] **PARSE-01**: Recursive descent parser produces complete AST for all Iron syntax
- [x] **PARSE-02**: Parser recovers from errors and reports multiple diagnostics per file
- [x] **PARSE-03**: String interpolation segments are parsed into AST nodes
- [x] **PARSE-04**: Operator precedence is correctly handled for all binary/unary operators
- [x] **PARSE-05**: AST pretty-printer can dump tree back to readable Iron for debugging

### Semantic Analysis

- [x] **SEM-01**: Name resolution builds scoped symbol table (global → module → function → block)
- [x] **SEM-02**: All identifiers resolve to declarations; undefined variables produce errors
- [x] **SEM-03**: Type inference works for val/var declarations without explicit types
- [x] **SEM-04**: Type checker validates all assignments, function calls, and return types
- [x] **SEM-05**: val immutability is enforced (reassignment produces compile error)
- [x] **SEM-06**: Nullable types require null check before use; compiler narrows type after check
- [x] **SEM-07**: Interface implementation completeness is validated
- [x] **SEM-08**: Generic type parameters are validated and instantiated
- [x] **SEM-09**: Escape analysis tracks heap allocations and marks non-escaping values for auto-free
- [x] **SEM-10**: Concurrency checks enforce parallel-for body cannot mutate outer non-mutex variables
- [x] **SEM-11**: Import resolution locates .iron files by path and builds module graph
- [x] **SEM-12**: `self` and `super` resolve correctly inside methods

### Code Generation

- [x] **GEN-01**: C code emitted for all Iron language constructs compiles with `gcc/clang -std=c11 -Wall -Werror`
- [x] **GEN-02**: Defer statements execute in reverse order at every scope exit including early returns
- [x] **GEN-03**: Object inheritance uses struct embedding; child pointer castable to parent
- [x] **GEN-04**: Interface dispatch uses vtable structs with function pointers
- [x] **GEN-05**: Generics monomorphized to concrete C types
- [x] **GEN-06**: Forward declarations and topological sort prevent C compilation order issues
- [x] **GEN-07**: Generated C uses consistent namespace prefix to prevent symbol collisions
- [x] **GEN-08**: Nullable types generate Optional structs with value + has_value flag
- [x] **GEN-09**: Lambda expressions generate C function pointers with closure data
- [x] **GEN-10**: Spawn/await/channel/mutex generate correct thread pool and synchronization code
- [x] **GEN-11**: Parallel-for generates range splitting, chunk submission, and barrier

### Runtime Library

- [x] **RT-01**: String type supports UTF-8 with interning and small string optimization
- [x] **RT-02**: List (dynamic array), Map (hash map), and Set (hash set) collections work correctly
- [x] **RT-03**: Reference counting (rc) correctly manages shared ownership with retain/release
- [x] **RT-04**: Thread pool implementation with work queue, submit, and barrier
- [x] **RT-05**: Channel implementation (ring buffer + mutex + condvars) with send/recv/try_recv
- [x] **RT-06**: Mutex wraps a value with lock semantics
- [x] **RT-07**: Built-in functions work: print, println, len, range, min, max, clamp, abs, assert
- [x] **RT-08**: Runtime compiles and passes tests on macOS, Linux, and Windows

### Standard Library

- [x] **STD-01**: math module provides trig, sqrt, pow, lerp, random, PI/TAU/E
- [x] **STD-02**: io module provides file read/write, file_exists, list_files, create_dir
- [x] **STD-03**: time module provides now, now_ms, sleep, since, Timer
- [x] **STD-04**: log module provides info/warn/error/debug with level filtering

### CLI Toolchain

- [x] **CLI-01**: `iron build [file]` compiles .iron to standalone binary via C
- [x] **CLI-02**: `iron run [file]` compiles and immediately executes
- [x] **CLI-03**: `iron check [file]` type-checks without compiling to binary
- [x] **CLI-04**: `iron fmt [file]` formats Iron source code
- [x] **CLI-05**: `iron test [dir]` discovers and runs Iron tests
- [x] **CLI-06**: Error messages show Rust-style diagnostics: source snippet, arrow, suggestion
- [x] **CLI-07**: Terminal output is colored
- [x] **CLI-08**: `--verbose` flag shows generated C code

### Comptime

- [x] **CT-01**: `comptime` expressions evaluate at compile time and emit result as literals
- [x] **CT-02**: `comptime read_file()` embeds file contents as string/byte array at compile time
- [x] **CT-03**: Comptime restrictions enforced: no heap, no rc, no runtime I/O
- [x] **CT-04**: Step limit prevents infinite loops during compile-time evaluation

### Game Dev Integration

- [x] **GAME-01**: Raylib bindings allow Iron programs to create windows, draw, and handle input
- [x] **GAME-02**: The draw {} block syntax works for raylib begin/end drawing
- [x] **GAME-03**: Compiled binaries are standalone executables (runtime statically linked)
- [x] **GAME-04**: Build produces working binaries on macOS, Linux, and Windows

### Testing

- [x] **TEST-01**: C unit tests cover all compiler internals (lexer, parser, semantic, codegen)
- [x] **TEST-02**: .iron integration tests verify end-to-end compilation and execution
- [x] **TEST-03**: Error diagnostic tests verify specific error messages for specific mistakes
- [x] **TEST-04**: Memory safety validated with ASan/UBSan in CI

## v2 Requirements

Deferred to future milestones. Tracked but not in current roadmap.

### Self-Hosting

- **SELF-01**: Iron compiler rewritten in Iron
- **SELF-02**: Compiler can compile itself

### Developer Tools

- **DEVT-01**: LSP server for editor integration
- **DEVT-02**: Debugger integration (DAP protocol)
- **DEVT-03**: IDE/editor plugins (VS Code, etc.)

### Package Management

- **PKG-01**: Package manager for Iron libraries
- **PKG-02**: Dependency resolution and versioning

### Additional Backends

- **BACK-01**: SDL2 bindings as alternative to raylib
- **BACK-02**: Vulkan/Metal bindings for advanced graphics

## Out of Scope

| Feature | Reason |
|---------|--------|
| Garbage collector | Conflicts with manual memory philosophy; game dev needs predictable performance |
| Borrow checker | Deliberate design choice; escape analysis + explicit tiers is Iron's approach |
| REPL / interpreter mode | Compiled language; comptime covers eval-at-build-time |
| JIT compilation | Adds massive complexity; standalone binaries are the goal |
| Operator overloading | Violates "legibility over magic" principle |
| Implicit type conversions | Explicitly excluded in language philosophy |
| Named arguments at call sites | Language spec explicitly uses positional-only |
| WASM target | Future consideration; native binaries first |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| LEX-01 | Phase 1 | Complete |
| LEX-02 | Phase 1 | Complete |
| LEX-03 | Phase 1 | Complete |
| LEX-04 | Phase 1 | Complete |
| PARSE-01 | Phase 1 | Complete |
| PARSE-02 | Phase 1 | Complete |
| PARSE-03 | Phase 1 | Complete |
| PARSE-04 | Phase 1 | Complete |
| PARSE-05 | Phase 1 | Complete |
| TEST-03 | Phase 1 | Complete |
| SEM-01 | Phase 2 | Complete |
| SEM-02 | Phase 2 | Complete |
| SEM-03 | Phase 2 | Complete |
| SEM-04 | Phase 2 | Complete |
| SEM-05 | Phase 2 | Complete |
| SEM-06 | Phase 2 | Complete |
| SEM-07 | Phase 2 | Complete |
| SEM-08 | Phase 2 | Complete |
| SEM-09 | Phase 2 | Complete |
| SEM-10 | Phase 2 | Complete |
| SEM-11 | Phase 2 | Complete |
| SEM-12 | Phase 2 | Complete |
| GEN-01 | Phase 2 | Complete |
| GEN-02 | Phase 2 | Complete |
| GEN-03 | Phase 2 | Complete |
| GEN-04 | Phase 2 | Complete |
| GEN-05 | Phase 2 | Complete |
| GEN-06 | Phase 2 | Complete |
| GEN-07 | Phase 2 | Complete |
| GEN-08 | Phase 2 | Complete |
| GEN-09 | Phase 2 | Complete |
| GEN-10 | Phase 2 | Complete |
| GEN-11 | Phase 5 | Complete |
| TEST-01 | Phase 2 | Complete |
| TEST-02 | Phase 2 | Complete |
| RT-01 | Phase 3 | Complete |
| RT-02 | Phase 3 | Complete |
| RT-03 | Phase 3 | Complete |
| RT-04 | Phase 3 | Complete |
| RT-05 | Phase 3 | Complete |
| RT-06 | Phase 3 | Complete |
| RT-07 | Phase 6 | Complete |
| RT-08 | Phase 3 | Complete |
| STD-01 | Phase 5 | Complete |
| STD-02 | Phase 5 | Complete |
| STD-03 | Phase 6 | Complete |
| STD-04 | Phase 5 | Complete |
| CLI-01 | Phase 3 | Complete |
| CLI-02 | Phase 3 | Complete |
| CLI-03 | Phase 6 | Complete |
| CLI-04 | Phase 3 | Complete |
| CLI-05 | Phase 3 | Complete |
| CLI-06 | Phase 3 | Complete |
| CLI-07 | Phase 3 | Complete |
| CLI-08 | Phase 3 | Complete |
| TEST-04 | Phase 3 | Complete |
| CT-01 | Phase 4 | Complete |
| CT-02 | Phase 4 | Complete |
| CT-03 | Phase 4 | Complete |
| CT-04 | Phase 4 | Complete |
| GAME-01 | Phase 4 | Complete |
| GAME-02 | Phase 4 | Complete |
| GAME-03 | Phase 4 | Complete |
| GAME-04 | Phase 4 | Complete |

**Coverage:**
- v1 requirements: 52 total
- Mapped to phases: 52
- Unmapped: 0

---
*Requirements defined: 2025-03-25*
*Last updated: 2026-03-26 — RT-07, STD-03, CLI-03 reassigned to Phase 6 gap closure*
