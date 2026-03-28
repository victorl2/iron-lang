# Requirements: Iron

**Defined:** 2025-03-25
**Core Value:** Every Iron language feature compiles to correct, working C code that produces a native binary

## v0.0.1-alpha Requirements (Validated)

All v0.0.1-alpha requirements shipped and verified. See MILESTONES.md for details.

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

- [x] **SEM-01**: Name resolution builds scoped symbol table (global -> module -> function -> block)
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

## v0.0.2-alpha Requirements (Validated)

Requirements for High IR milestone. All shipped and verified.

### IR Core

- [x] **IRCORE-01**: IR data structures defined (IrValue, IrInstr, IrBasicBlock, IrFunction, IrModule) with SSA-form semantics
- [x] **IRCORE-02**: IR instructions use integer value IDs (not pointers) for operand references
- [x] **IRCORE-03**: IR carries full Iron type system (Iron_Type*) without introducing IR-specific types
- [x] **IRCORE-04**: Each IR instruction carries an Iron_Span for source location tracking

### Instructions

- [x] **INSTR-01**: Const instructions for int, float, bool, string, and null literals
- [x] **INSTR-02**: Arithmetic instructions (add, sub, mul, div, mod) and comparison instructions (eq, neq, lt, lte, gt, gte)
- [x] **INSTR-03**: Unary instructions (negate, not)
- [x] **INSTR-04**: Alloca/load/store instructions for mutable variable bindings
- [x] **INSTR-05**: Field get/set instructions for object member access
- [x] **INSTR-06**: Array index get/set instructions
- [x] **INSTR-07**: Object/struct construction instruction
- [x] **INSTR-08**: Array literal instruction
- [x] **INSTR-09**: Type cast instruction for numeric conversions and IS expressions
- [x] **INSTR-10**: Nullable check instructions (is_null/is_not_null) and narrowed access
- [x] **INSTR-11**: String interpolation instruction (high-level, opaque to emitter)
- [x] **INSTR-12**: Slice expression instruction
- [x] **INSTR-13**: Function call instruction for direct, indirect, and method calls

### Control Flow

- [x] **CTRL-01**: Conditional branch terminator (branch on bool to then/else blocks)
- [x] **CTRL-02**: Unconditional jump terminator
- [x] **CTRL-03**: Return terminator (with value or void)
- [x] **CTRL-04**: Switch/match terminator for enum pattern matching

### Memory

- [x] **MEM-01**: Heap allocation instruction with auto_free and escapes flags from escape analysis
- [x] **MEM-02**: Rc allocation instruction
- [x] **MEM-03**: Free instruction (explicit free)
- [x] **MEM-04**: Defer statements eagerly lowered during AST->IR (deferred calls inlined before every exit point)

### Concurrency

- [x] **CONC-01**: Lambda/closure lifting to top-level IrFunction with environment struct
- [x] **CONC-02**: Spawn statement lifting to top-level IrFunction with pool submission
- [x] **CONC-03**: Parallel-for lowering with chunk function lifting and capture analysis
- [x] **CONC-04**: Await instruction for blocking on spawned tasks

### Module

- [x] **MOD-01**: Module-level type declarations (objects, enums, interfaces) with correct vtable ordering
- [x] **MOD-02**: Extern function declarations with C-level names
- [x] **MOD-03**: Monomorphization registry on IrModule for generic type deduplication
- [x] **MOD-04**: IRON_NODE_DRAW removed from parser; draw functionality moved to raylib.draw() module function

### Emission

- [x] **EMIT-01**: C emission backend that consumes IrModule and produces equivalent C to old codegen
- [x] **EMIT-02**: All existing integration tests pass through the new AST->IR->C pipeline
- [x] **EMIT-03**: Old AST->C codegen fully removed after parity verification

### Tooling

- [x] **TOOL-01**: IR printer producing human-readable text dump of IR modules
- [x] **TOOL-02**: IR verifier validating structural invariants (values defined before use, blocks have terminators, branch targets valid)

### Test Hardening

- [x] **THARD-01**: Test directory reorganized into clear structure (unit/, integration/, algorithms/, ir/) with consistent naming conventions
- [x] **THARD-02**: Real algorithm test suite — quicksort, subset sum, binary search, fibonacci (recursive + iterative), merge sort, BFS/DFS graph traversal — each compiles and produces correct output through IR pipeline
- [x] **THARD-03**: IR lowering unit tests cover every instruction kind with targeted .iron snippets that exercise each IR path in isolation
- [x] **THARD-04**: IR verifier negative tests — malformed IR triggers specific verification errors for every invariant the verifier checks
- [x] **THARD-05**: Control flow edge case tests — deeply nested if/else (5+ levels), while with break/continue patterns, match with fallthrough, multiple early returns with defer cleanup
- [x] **THARD-06**: Memory and ownership test suite — heap auto_free in loops, rc cycle patterns, defer with multiple exit paths, explicit free after conditional
- [x] **THARD-07**: Concurrency test suite — lambda capturing mutable vars, spawn with shared state, parallel-for with captured collections, nested spawn/await
- [x] **THARD-08**: Generic and polymorphism stress tests — multiple instantiations of same generic type, interface dispatch through generic containers, nested generics
- [x] **THARD-09**: Real-world composite programs — a mini game loop (raylib), a CLI tool (file processing), a concurrent data pipeline — each exercising multiple language features together
- [x] **THARD-10**: IR printer snapshot tests — printed IR for representative programs is captured and diffed against expected output to catch IR regressions

### Release & Versioning

- [x] **REL-01**: CI builds release binaries for macOS (arm64, x86_64), Linux (x86_64), and Windows (x86_64) when a draft GitHub release is created
- [x] **REL-02**: Release binaries are uploaded as downloadable assets on the GitHub release page (ready-to-use iron binary)
- [x] **REL-03**: `iron --version` outputs version string with commit hash and build date in rustc format (e.g., `iron 0.1.1 (abc1234 2026-03-27)`)
- [x] **REL-04**: Build date and commit hash are baked in at compile time via CMake configure or build flags

## v0.0.3-alpha Requirements

Requirements for the Package Manager milestone. Each maps to roadmap phases.

### CLI Foundation

- [x] **CLI2-01**: Current compiler binary renamed to `ironc` with all existing subcommands (build, run, check, fmt, test)
- [x] **CLI2-02**: New `iron` CLI binary as package manager that invokes `ironc` behind the scenes
- [x] **CLI2-03**: `iron` auto-detects context: if `iron.toml` exists use package mode, if given a `.iron` file forward to `ironc`
- [ ] **CLI2-04**: Both `iron` and `ironc` installed together and added to PATH via the install script
- [ ] **CLI2-05**: Colored CLI output — Cargo-style colors (green success, red errors, cyan info), respects `NO_COLOR` env var and `isatty()`

### Project Management

- [ ] **PROJ-01**: `iron init` scaffolds a binary project in the current directory (iron.toml + src/main.iron)
- [ ] **PROJ-02**: `iron init --lib` scaffolds a library project (iron.toml + src/lib.iron)
- [ ] **PROJ-03**: `iron build` reads iron.toml, discovers source files, and builds the package via ironc
- [ ] **PROJ-04**: `iron run` builds and runs the current package
- [ ] **PROJ-05**: `iron check` analyzes the package and reports errors without producing object files
- [ ] **PROJ-06**: `iron test` discovers and runs all tests in the package

### Dependencies

- [ ] **DEP-01**: `iron.toml` supports Cargo-style `[package]` (name, version, authors, type) and `[dependencies]` sections
- [ ] **DEP-02**: Dependencies specified as `name = { git = "user/repo", version = "X.Y.Z" }` in iron.toml
- [ ] **DEP-03**: Source-based dependency resolution — download source from GitHub, compile locally
- [ ] **DEP-04**: `iron.lock` lockfile with `lock_version = 1`, commit SHAs, for reproducible builds
- [ ] **DEP-05**: Transitive dependency resolution with cycle detection

## Future Requirements

Deferred to future milestones. Tracked but not in current roadmap.

### Optimization

- **OPT-01**: Dead code elimination pass
- **OPT-02**: Constant folding pass
- **OPT-03**: Common subexpression elimination
- **OPT-04**: Def-use chain tracking for optimization passes

### LLVM Backend

- **LLVM-01**: LLVM IR emission from High IR
- **LLVM-02**: Low IR representation (LLVM-level)

### SSA Improvements

- **SSA-01**: Block parameters instead of phi nodes
- **SSA-02**: Mem2reg pass to promote allocas to pure SSA values

### Self-Hosting

- **SELF-01**: Iron compiler rewritten in Iron
- **SELF-02**: Compiler can compile itself

### Developer Tools

- **DEVT-01**: LSP server for editor integration
- **DEVT-02**: Debugger integration (DAP protocol)
- **DEVT-03**: IDE/editor plugins (VS Code, etc.)

### Package Management (Extended)

- **PKG-01**: `iron update` command to update dependencies to latest compatible versions
- **PKG-02**: `iron install` command to install a package as a global binary
- **PKG-03**: Local path dependencies (`path = "../my-lib"`)
- **PKG-04**: Dependency registry/index (beyond GitHub-based resolution)
- **PKG-05**: Workspace/monorepo support for multi-package projects
- **PKG-06**: Build scripts (build.iron) for custom build steps

### Additional Backends

- **BACK-01**: SDL2 bindings as alternative to raylib
- **BACK-02**: Vulkan/Metal bindings for advanced graphics

## Out of Scope

Explicitly excluded. Documented to prevent scope creep.

| Feature | Reason |
|---------|--------|
| Semver range resolution | Ecosystem too small for version conflicts; exact pinning sufficient |
| Package registry | GitHub-based resolution is sufficient for alpha |
| C/system dependency management | Iron-only deps for now; system deps are a different problem |
| Workspaces/monorepos | Single package first; workspaces add significant complexity |
| Build scripts | Premature; no use cases yet in the Iron ecosystem |
| Private repo auth | Adds credential management complexity; public repos for alpha |
| Phi node placement (dominance frontiers) | Alloca model avoids this entirely; add via mem2reg in future |
| IR optimization passes | Must have correct pipeline first; explicit future milestone |
| LLVM IR emission | High IR designed to enable this; explicit future milestone |
| Garbage collector | Conflicts with manual memory philosophy; game dev needs predictable performance |
| Borrow checker | Deliberate design choice; escape analysis + explicit tiers is Iron's approach |
| Operator overloading | Violates "legibility over magic" principle |
| Implicit type conversions | Explicitly excluded in language philosophy |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

### v0.0.1-alpha (Complete)

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

### v0.0.2-alpha (Complete)

| Requirement | Phase | Status |
|-------------|-------|--------|
| IRCORE-01 | Phase 7 | Complete |
| IRCORE-02 | Phase 7 | Complete |
| IRCORE-03 | Phase 7 | Complete |
| IRCORE-04 | Phase 7 | Complete |
| INSTR-01 | Phase 8 | Complete |
| INSTR-02 | Phase 8 | Complete |
| INSTR-03 | Phase 8 | Complete |
| INSTR-04 | Phase 8 | Complete |
| INSTR-05 | Phase 8 | Complete |
| INSTR-06 | Phase 8 | Complete |
| INSTR-07 | Phase 8 | Complete |
| INSTR-08 | Phase 8 | Complete |
| INSTR-09 | Phase 8 | Complete |
| INSTR-10 | Phase 8 | Complete |
| INSTR-11 | Phase 8 | Complete |
| INSTR-12 | Phase 8 | Complete |
| INSTR-13 | Phase 8 | Complete |
| CTRL-01 | Phase 8 | Complete |
| CTRL-02 | Phase 8 | Complete |
| CTRL-03 | Phase 8 | Complete |
| CTRL-04 | Phase 8 | Complete |
| MEM-01 | Phase 8 | Complete |
| MEM-02 | Phase 8 | Complete |
| MEM-03 | Phase 8 | Complete |
| MEM-04 | Phase 8 | Complete |
| CONC-01 | Phase 8 | Complete |
| CONC-02 | Phase 8 | Complete |
| CONC-03 | Phase 8 | Complete |
| CONC-04 | Phase 8 | Complete |
| MOD-01 | Phase 8 | Complete |
| MOD-02 | Phase 8 | Complete |
| MOD-03 | Phase 8 | Complete |
| MOD-04 | Phase 8 | Complete |
| EMIT-01 | Phase 9 | Complete |
| EMIT-02 | Phase 9 | Complete |
| EMIT-03 | Phase 9 | Complete |
| TOOL-01 | Phase 7 | Complete |
| TOOL-02 | Phase 7 | Complete |
| THARD-01 | Phase 10 | Complete |
| THARD-02 | Phase 10 | Complete |
| THARD-03 | Phase 10 | Complete |
| THARD-04 | Phase 10 | Complete |
| THARD-05 | Phase 10 | Complete |
| THARD-06 | Phase 10 | Complete |
| THARD-07 | Phase 10 | Complete |
| THARD-08 | Phase 10 | Complete |
| THARD-09 | Phase 10 | Complete |
| THARD-10 | Phase 10 | Complete |
| REL-01 | Phase 11 | Complete |
| REL-02 | Phase 11 | Complete |
| REL-03 | Phase 11 | Complete |
| REL-04 | Phase 11 | Complete |

### v0.0.3-alpha (Active)

| Requirement | Phase | Status |
|-------------|-------|--------|
| CLI2-01 | Phase 12 | Complete |
| CLI2-02 | Phase 12 | Complete |
| CLI2-03 | Phase 12 | Complete |
| CLI2-04 | Phase 12 | Pending |
| CLI2-05 | Phase 13 | Pending |
| PROJ-01 | Phase 13 | Pending |
| PROJ-02 | Phase 13 | Pending |
| PROJ-03 | Phase 13 | Pending |
| PROJ-04 | Phase 13 | Pending |
| PROJ-05 | Phase 13 | Pending |
| PROJ-06 | Phase 13 | Pending |
| DEP-01 | Phase 13 | Pending |
| DEP-02 | Phase 13 | Pending |
| DEP-03 | Phase 14 | Pending |
| DEP-04 | Phase 14 | Pending |
| DEP-05 | Phase 14 | Pending |

**Coverage:**
- v0.0.1-alpha requirements: 52 total — all Complete
- v0.0.2-alpha requirements: 52 total — all Complete
- v0.0.3-alpha requirements: 16 total
- Mapped to phases: 16
- Unmapped: 0

---
*Requirements defined: 2025-03-25*
*Last updated: 2026-03-27 after v0.0.3-alpha roadmap created (Phases 12-14)*
