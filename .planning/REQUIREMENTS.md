# Requirements: Iron

**Defined:** 2025-03-25
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v1 Requirements

Requirements for initial release. Each maps to roadmap phases.

### Lexer

- [ ] **LEX-01**: Compiler tokenizes all Iron keywords, operators, literals, and delimiters
- [ ] **LEX-02**: Every token carries source span (file, line, column) for diagnostics
- [ ] **LEX-03**: Lexer reports errors for unterminated strings and invalid characters with location
- [ ] **LEX-04**: Comments (-- to end of line) are recognized and skipped

### Parser

- [ ] **PARSE-01**: Recursive descent parser produces complete AST for all Iron syntax
- [ ] **PARSE-02**: Parser recovers from errors and reports multiple diagnostics per file
- [ ] **PARSE-03**: String interpolation segments are parsed into AST nodes
- [ ] **PARSE-04**: Operator precedence is correctly handled for all binary/unary operators
- [ ] **PARSE-05**: AST pretty-printer can dump tree back to readable Iron for debugging

### Semantic Analysis

- [ ] **SEM-01**: Name resolution builds scoped symbol table (global → module → function → block)
- [ ] **SEM-02**: All identifiers resolve to declarations; undefined variables produce errors
- [ ] **SEM-03**: Type inference works for val/var declarations without explicit types
- [ ] **SEM-04**: Type checker validates all assignments, function calls, and return types
- [ ] **SEM-05**: val immutability is enforced (reassignment produces compile error)
- [ ] **SEM-06**: Nullable types require null check before use; compiler narrows type after check
- [ ] **SEM-07**: Interface implementation completeness is validated
- [ ] **SEM-08**: Generic type parameters are validated and instantiated
- [ ] **SEM-09**: Escape analysis tracks heap allocations and marks non-escaping values for auto-free
- [ ] **SEM-10**: Concurrency checks enforce parallel-for body cannot mutate outer non-mutex variables
- [ ] **SEM-11**: Import resolution locates .iron files by path and builds module graph
- [ ] **SEM-12**: `self` and `super` resolve correctly inside methods

### Code Generation

- [ ] **GEN-01**: C code emitted for all Iron language constructs compiles with `gcc/clang -std=c11 -Wall -Werror`
- [ ] **GEN-02**: Defer statements execute in reverse order at every scope exit including early returns
- [ ] **GEN-03**: Object inheritance uses struct embedding; child pointer castable to parent
- [ ] **GEN-04**: Interface dispatch uses vtable structs with function pointers
- [ ] **GEN-05**: Generics monomorphized to concrete C types
- [ ] **GEN-06**: Forward declarations and topological sort prevent C compilation order issues
- [ ] **GEN-07**: Generated C uses consistent namespace prefix to prevent symbol collisions
- [ ] **GEN-08**: Nullable types generate Optional structs with value + has_value flag
- [ ] **GEN-09**: Lambda expressions generate C function pointers with closure data
- [ ] **GEN-10**: Spawn/await/channel/mutex generate correct thread pool and synchronization code
- [ ] **GEN-11**: Parallel-for generates range splitting, chunk submission, and barrier

### Runtime Library

- [ ] **RT-01**: String type supports UTF-8 with interning and small string optimization
- [ ] **RT-02**: List (dynamic array), Map (hash map), and Set (hash set) collections work correctly
- [ ] **RT-03**: Reference counting (rc) correctly manages shared ownership with retain/release
- [ ] **RT-04**: Thread pool implementation with work queue, submit, and barrier
- [ ] **RT-05**: Channel implementation (ring buffer + mutex + condvars) with send/recv/try_recv
- [ ] **RT-06**: Mutex wraps a value with lock semantics
- [ ] **RT-07**: Built-in functions work: print, println, len, range, min, max, clamp, abs, assert
- [ ] **RT-08**: Runtime compiles and passes tests on macOS, Linux, and Windows

### Standard Library

- [ ] **STD-01**: math module provides trig, sqrt, pow, lerp, random, PI/TAU/E
- [ ] **STD-02**: io module provides file read/write, file_exists, list_files, create_dir
- [ ] **STD-03**: time module provides now, now_ms, sleep, since, Timer
- [ ] **STD-04**: log module provides info/warn/error/debug with level filtering

### CLI Toolchain

- [ ] **CLI-01**: `iron build [file]` compiles .iron to standalone binary via C
- [ ] **CLI-02**: `iron run [file]` compiles and immediately executes
- [ ] **CLI-03**: `iron check [file]` type-checks without compiling to binary
- [ ] **CLI-04**: `iron fmt [file]` formats Iron source code
- [ ] **CLI-05**: `iron test [dir]` discovers and runs Iron tests
- [ ] **CLI-06**: Error messages show Rust-style diagnostics: source snippet, arrow, suggestion
- [ ] **CLI-07**: Terminal output is colored
- [ ] **CLI-08**: `--verbose` flag shows generated C code

### Comptime

- [ ] **CT-01**: `comptime` expressions evaluate at compile time and emit result as literals
- [ ] **CT-02**: `comptime read_file()` embeds file contents as string/byte array at compile time
- [ ] **CT-03**: Comptime restrictions enforced: no heap, no rc, no runtime I/O
- [ ] **CT-04**: Step limit prevents infinite loops during compile-time evaluation

### Game Dev Integration

- [ ] **GAME-01**: Raylib bindings allow Iron programs to create windows, draw, and handle input
- [ ] **GAME-02**: The draw {} block syntax works for raylib begin/end drawing
- [ ] **GAME-03**: Compiled binaries are standalone executables (runtime statically linked)
- [ ] **GAME-04**: Build produces working binaries on macOS, Linux, and Windows

### Testing

- [ ] **TEST-01**: C unit tests cover all compiler internals (lexer, parser, semantic, codegen)
- [ ] **TEST-02**: .iron integration tests verify end-to-end compilation and execution
- [ ] **TEST-03**: Error diagnostic tests verify specific error messages for specific mistakes
- [ ] **TEST-04**: Memory safety validated with ASan/UBSan in CI

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
| LEX-01 | Phase 1 | Pending |
| LEX-02 | Phase 1 | Pending |
| LEX-03 | Phase 1 | Pending |
| LEX-04 | Phase 1 | Pending |
| PARSE-01 | Phase 2 | Pending |
| PARSE-02 | Phase 2 | Pending |
| PARSE-03 | Phase 2 | Pending |
| PARSE-04 | Phase 2 | Pending |
| PARSE-05 | Phase 2 | Pending |
| SEM-01 | Phase 3 | Pending |
| SEM-02 | Phase 3 | Pending |
| SEM-03 | Phase 3 | Pending |
| SEM-04 | Phase 3 | Pending |
| SEM-05 | Phase 3 | Pending |
| SEM-06 | Phase 3 | Pending |
| SEM-07 | Phase 3 | Pending |
| SEM-08 | Phase 3 | Pending |
| SEM-09 | Phase 3 | Pending |
| SEM-10 | Phase 3 | Pending |
| SEM-11 | Phase 3 | Pending |
| SEM-12 | Phase 3 | Pending |
| GEN-01 | Phase 4 | Pending |
| GEN-02 | Phase 4 | Pending |
| GEN-03 | Phase 4 | Pending |
| GEN-04 | Phase 4 | Pending |
| GEN-05 | Phase 4 | Pending |
| GEN-06 | Phase 4 | Pending |
| GEN-07 | Phase 4 | Pending |
| GEN-08 | Phase 4 | Pending |
| GEN-09 | Phase 4 | Pending |
| GEN-10 | Phase 4 | Pending |
| GEN-11 | Phase 4 | Pending |
| RT-01 | Phase 5 | Pending |
| RT-02 | Phase 5 | Pending |
| RT-03 | Phase 5 | Pending |
| RT-04 | Phase 5 | Pending |
| RT-05 | Phase 5 | Pending |
| RT-06 | Phase 5 | Pending |
| RT-07 | Phase 5 | Pending |
| RT-08 | Phase 5 | Pending |
| STD-01 | Phase 5 | Pending |
| STD-02 | Phase 5 | Pending |
| STD-03 | Phase 5 | Pending |
| STD-04 | Phase 5 | Pending |
| CLI-01 | Phase 5 | Pending |
| CLI-02 | Phase 5 | Pending |
| CLI-03 | Phase 5 | Pending |
| CLI-04 | Phase 5 | Pending |
| CLI-05 | Phase 5 | Pending |
| CLI-06 | Phase 5 | Pending |
| CLI-07 | Phase 5 | Pending |
| CLI-08 | Phase 5 | Pending |
| CT-01 | Phase 5 | Pending |
| CT-02 | Phase 5 | Pending |
| CT-03 | Phase 5 | Pending |
| CT-04 | Phase 5 | Pending |
| GAME-01 | Phase 5 | Pending |
| GAME-02 | Phase 5 | Pending |
| GAME-03 | Phase 5 | Pending |
| GAME-04 | Phase 5 | Pending |
| TEST-01 | Phase 1-5 | Pending |
| TEST-02 | Phase 1-5 | Pending |
| TEST-03 | Phase 1-5 | Pending |
| TEST-04 | Phase 1-5 | Pending |

**Coverage:**
- v1 requirements: 52 total
- Mapped to phases: 52
- Unmapped: 0

---
*Requirements defined: 2025-03-25*
*Last updated: 2025-03-25 after initial definition*
