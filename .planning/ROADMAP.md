# Roadmap: Iron

## Milestones

- v0.0.1-alpha Iron Language Compiler - Phases 1-6 (shipped 2026-03-27)
- v0.0.2-alpha High IR - Phases 7-11 (shipped 2026-03-27)
- v0.0.3-alpha Package Manager - Phases 12-14 (shipped 2026-03-28)
- v0.0.5-alpha IR Optimization & High IR Architecture - Phases 15-20 (shipped 2026-03-30)
- v0.0.6-alpha HIR Pipeline Correctness - Phases 21-23 (shipped 2026-03-31)
- v0.0.7-alpha Performance Optimization - Phases 24-31 (shipped 2026-04-01)
- v0.0.8-alpha Semantic Analysis Gaps - Phases 32-39 (shipped 2026-04-04)
- v0.1-alpha Static Interface Dispatch - Phases 40-45 (shipped 2026-04-07)
- v0.1.1-alpha Collection Methods, Full Captures & Layout Optimizations - Phases 46-50 (shipped 2026-04-08)
- v0.1.2-alpha Compiler Hardening & Refactoring - Phases 52-54 (shipped 2026-04-09)
- v0.1.3-alpha Known Limitations Cleanup - Phases 55-58 (active)

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

<details>
<summary>v0.0.1-alpha Iron Language Compiler (Phases 1-6) - SHIPPED 2026-03-27</summary>

- [x] **Phase 1: Frontend** - Lexer and parser produce a complete, span-annotated AST from any Iron source file (completed 2026-03-25)
- [x] **Phase 2: Semantics and Codegen** - Fully analyzed AST emits valid C11 that compiles and runs (completed 2026-03-26)
- [x] **Phase 3: Runtime, Stdlib, and CLI** - Iron programs are buildable, runnable, and testable from the command line (completed 2026-03-26)
- [x] **Phase 4: Comptime, Game Dev, and Cross-Platform** - Comptime evaluation, raylib bindings, and Windows parity complete v1 (completed 2026-03-26)
- [x] **Phase 5: Codegen Fixes + Stdlib Wiring** - Fix string interpolation and parallel-for codegen; wire stdlib modules to Iron source via import (completed 2026-03-26)
- [x] **Phase 6: Milestone Gap Closure** - Close remaining v1.0 audit gaps: range builtin, Timer wrappers, iron check stdlib support (completed 2026-03-27)

</details>

<details>
<summary>v0.0.2-alpha High IR (Phases 7-11) - SHIPPED 2026-03-27</summary>

- [x] **Phase 7: IR Foundation** - IR data structures, instruction set definition, printer, and verifier provide the complete scaffold for lowering and emission
- [x] **Phase 8: AST-to-IR Lowering** - Every Iron language feature lowers from AST to typed SSA-form IR instructions
- [x] **Phase 9: C Emission and Cutover** - IR-to-C backend produces equivalent output to old codegen; old codegen removed after parity gate
- [x] **Phase 10: Test Hardening** - Reorganize test structure, add real algorithm tests, comprehensive IR coverage tests, and real-world composite programs
- [x] **Phase 11: Release Pipeline & Versioning** - CI builds release binaries on draft release; `iron --version` shows commit hash and build date

</details>

<details>
<summary>v0.0.3-alpha Package Manager (Phases 12-14) - SHIPPED 2026-03-28</summary>

- [x] **Phase 12: Binary Split and Installation** - `ironc` and `iron` are two separate installed binaries; existing single-file workflows continue to work unchanged (completed 2026-03-28)
- [x] **Phase 13: Project Workflow** - `iron init`, `iron build`, `iron run`, `iron check`, `iron test` work for iron.toml projects; colored Cargo-style output on all platforms (completed 2026-03-28)
- [x] **Phase 14: Dependency Resolution and Lockfile** - GitHub-sourced dependencies are fetched, compiled, and pinned in iron.lock with reproducible commit SHAs (completed 2026-03-28)

</details>

<details>
<summary>v0.0.5-alpha IR Optimization & High IR Architecture (Phases 15-20) - SHIPPED 2026-03-30</summary>

- [x] **Phase 15: Copy Propagation, DCE & Constant Folding** - Simpler optimization passes in a new ir_optimize.c module eliminate redundant copies, dead instructions, and compile-time constants
- [x] **Phase 16: Expression Inlining** - Reconstruct compound C expressions during emission for single-use pure values, closing the largest performance gap
- [x] **Phase 17: Strength Reduction & Store/Load Elimination** - Replace expensive loop index patterns with induction variables and remove redundant memory operations
- [x] **Phase 18: Benchmark Validation** - All 127 benchmarks pass at their configured parity thresholds with 100% pass rate
- [x] **Phase 19: LIR Rename & HIR Foundation** - Current IR renamed to Lower IR throughout codebase; High IR data structures, printer, and verifier established
- [x] **Phase 20: HIR Lowering & Pipeline Cutover** - AST-to-HIR and HIR-to-LIR lowering passes replace the direct AST-to-LIR path

</details>

<details>
<summary>v0.0.6-alpha HIR Pipeline Correctness (Phases 21-23) - SHIPPED 2026-03-31</summary>

- [x] **Phase 21: Parallel-For Fix** - pfor integration tests and pfor-dependent algorithm tests all compile and pass after fixing `is_lifted_func()` prefix recognition (completed 2026-03-31)
- [x] **Phase 22: Struct Codegen Fix** - Spurious constructor wrapping and excess CONSTRUCT field emission are eliminated; struct-dependent algorithm tests produce correct output (completed 2026-03-31)
- [x] **Phase 23: Correctness Audit** - Systematic HIR-to-LIR-to-C audit identifies and fixes any remaining correctness gaps; new test cases cover all problem areas (completed 2026-03-31)

</details>

<details>
<summary>v0.0.7-alpha Performance Optimization (Phases 24-31) - SHIPPED 2026-04-01</summary>

- [x] **Phase 24: Range Bound Hoisting** - `Iron_range()` is evaluated once in the loop pre-header; all for-range benchmarks show measurable speedup
- [x] **Phase 25: Stack Array Promotion** - `fill(CONST, val)` with constant size promotes to `alloca()`-based stack allocation for non-escaping small arrays
- [x] **Phase 26: LOAD Expression Inlining** - LOAD instructions are inline-eligible when their use is in the same block; cross-block LOADs remain excluded
- [x] **Phase 27: Function Inlining** - Small pure non-recursive functions are inlined at LIR level before the copy-prop/DCE fixpoint
- [x] **Phase 28: Phi Elimination Improvement** - Copy coalescing in phi elimination reduces generated temporaries in complex control flow
- [x] **Phase 29: Sized Integers** - `Int32` type annotation emits `int32_t` in generated C; array operations use 32-bit memory bandwidth
- [x] **Phase 30: Benchmark Validation and Exploration** - Full benchmark suite run against pre-optimization baseline; exploration pass identifies any remaining opportunities
- [x] **Phase 31: Spawn/Await Correctness** - Verify spawn/await semantics work end-to-end; fix concurrency benchmarks for fair timing

</details>

<details>
<summary>v0.0.8-alpha Semantic Analysis Gaps (Phases 32-39) - SHIPPED 2026-04-04</summary>

- [x] **Phase 32: LIR Verifier Hardening** - PHI type consistency and call argument validation catch type mismatches in the LIR before C emission
- [x] **Phase 33: Type Validation Checks** - Match exhaustiveness, cast safety, string interpolation stringability, and compound overflow checks catch invalid code in the type checker
- [x] **Phase 34: Bounds Checking** - Constant array index and slice bounds are validated at compile time against known sizes
- [x] **Phase 35: Escape Analysis Extension** - Field assignment, array index assignment, and function argument passing are tracked for heap escape
- [x] **Phase 36: Definite Assignment Analysis** - Variables used before initialization on any control flow path produce a compile error
- [x] **Phase 37: Generic Constraint Checking** - Concrete type arguments that violate declared generic constraints are rejected at instantiation sites
- [x] **Phase 38: Concurrency Safety** - Mutation detection in parallel/spawn blocks covers field access, array index, and read-write race patterns
- [x] **Phase 39: Diagnostic Test Sweep** - Every new diagnostic has positive and negative tests; complex analyses have edge-case coverage

</details>

### v0.1-alpha Static Interface Dispatch

- [ ] **Phase 40: Syntax & Core Dispatch** - `impl` keyword, whole-program implementor collection, tagged union construction, tag-based dispatch, vtable removal, and dead implementor elimination
- [ ] **Phase 41: Collection Splitting** - `List<Interface>` splits into per-type homogeneous arrays with ordered/unordered iteration strategy and `unordered` annotation
- [ ] **Phase 42: Memory Layout** - AoS/SoA field-level layout selection, dead field elimination, common field factoring, small/large variant split, and layout annotations
- [ ] **Phase 43: Loop Fusion & Monomorphic Specialization** - Chained collection operations fuse into single loops; single-type collections collapse to plain typed arrays
- [ ] **Phase 44: Hardware Acceleration & Value Optimization** - Per-type arena allocation, prefetch insertion, OpenMP parallelism, and value range compression
- [ ] **Phase 45: Documentation & Branding** - Language guide, internals docs, game-dev examples, and site branding update

### v0.1.1-alpha Collection Methods, Full Captures & Layout Optimizations

- [ ] **Phase 46: Full Closure Capture** - Mutable var capture by reference, closures returned from functions, closures as fields, nested and recursive lambdas, shared mutable state
- [x] **Phase 47: Collection Methods** - map, filter, reduce, forEach, sum on arrays with lambda arguments, method chaining, and interface-typed split collection dispatch
- [ ] **Phase 48: Layout Optimizations** - AoS/SoA field-level layout selection, dead field elimination, common field factoring, small/large variant split, layout annotations
- [ ] **Phase 49: Loop Fusion & Monomorphic Specialization** - Chained collection operations fuse into single pass per type; single-type collections collapse to plain typed arrays
- [ ] **Phase 50: Value Range Compression & Arena Allocation** - Fields narrowed to smallest sufficient type with widening reads/narrowing writes; per-type arena allocation with geometric growth

### v0.1.2-alpha Compiler Hardening & Refactoring

- [ ] **Phase 52: Emitter Refactoring** - Extract emit_c.c into focused sub-modules: split collection, fusion, struct/layout emission, and EmitCtx cleanup
- [ ] **Phase 53: Analysis Improvements** - Interprocedural monomorphic detection and value range analysis across function boundaries and conditional branches
- [x] **Phase 54: Test Hardening** - Edge case, stress, and optimization composition tests for comprehensive regression coverage

## Phase Details

<details>
<summary>v0.0.1-alpha Phase Details (Phases 1-6)</summary>

### Phase 1: Frontend
**Goal**: The compiler can ingest any valid Iron source file and produce a complete, error-annotated AST with source spans on every node
**Depends on**: Nothing (first phase)
**Requirements**: LEX-01, LEX-02, LEX-03, LEX-04, PARSE-01, PARSE-02, PARSE-03, PARSE-04, PARSE-05, TEST-03
**Success Criteria** (what must be TRUE):
  1. The lexer tokenizes every Iron keyword, operator, literal, delimiter, and comment in the language spec with no unrecognized input left behind
  2. Every token and every AST node carries a source span (file, line, column) that diagnostics can reference
  3. Lexer errors (unterminated strings, invalid characters) report exact location; a source file with 3 independent lex errors produces exactly 3 error messages
  4. The parser produces a complete AST for any syntactically valid Iron file, including string interpolation and all operator precedences
  5. A source file with 3 independent syntax errors produces exactly 3 diagnostics (not cascades); `ErrorNode` recovery lets parsing continue past each error
**Plans:** 4/4 plans complete

Plans:
- [x] 01-01-PLAN.md
- [x] 01-02-PLAN.md
- [x] 01-03-PLAN.md
- [x] 01-04-PLAN.md

### Phase 2: Semantics and Codegen
**Goal**: The compiler's analysis passes fully annotate the AST and the code generator emits C11 that compiles and executes correctly
**Depends on**: Phase 1
**Requirements**: SEM-01, SEM-02, SEM-03, SEM-04, SEM-05, SEM-06, SEM-07, SEM-08, SEM-09, SEM-10, SEM-11, SEM-12, GEN-01, GEN-02, GEN-03, GEN-04, GEN-05, GEN-06, GEN-07, GEN-08, GEN-09, GEN-10, GEN-11, TEST-01, TEST-02
**Success Criteria** (what must be TRUE):
  1. A valid Iron program that uses variables, functions, objects, generics, interfaces, and null safety compiles to C that passes `clang -std=c11 -Wall -Werror` with zero warnings
  2. A program using `defer` with multiple early returns executes all deferred calls in reverse order on every exit path; ASan shows zero leaks
  3. `val` reassignment, use of a nullable without a null check, and a missing interface method each produce a compile error pointing to the offending line
  4. Escape analysis marks non-escaping heap allocations and the generated C inserts the correct free; concurrency checks reject `parallel for` bodies that mutate outer non-mutex variables
  5. C unit tests cover lexer, parser, semantic, and codegen internals; end-to-end .iron integration tests verify compilation and execution
**Plans:** 8/8 plans complete

Plans:
- [x] 02-01-PLAN.md
- [x] 02-02-PLAN.md
- [x] 02-03-PLAN.md
- [x] 02-04-PLAN.md
- [x] 02-05-PLAN.md
- [x] 02-06-PLAN.md
- [x] 02-07-PLAN.md
- [x] 02-08-PLAN.md

### Phase 3: Runtime, Stdlib, and CLI
**Goal**: Iron programs can be built, run, checked, formatted, and tested via the `iron` CLI, backed by a complete runtime and standard library
**Depends on**: Phase 2
**Requirements**: RT-01, RT-02, RT-03, RT-04, RT-05, RT-06, RT-07, RT-08, STD-01, STD-02, STD-03, STD-04, CLI-01, CLI-02, CLI-03, CLI-04, CLI-05, CLI-06, CLI-07, CLI-08, TEST-04
**Success Criteria** (what must be TRUE):
  1. `iron build hello.iron` produces a standalone native binary that executes; `iron run hello.iron` does the same in one step
  2. String, List, Map, Set, and Rc work correctly in Iron programs; the runtime passes all tests on macOS and Linux
  3. Standard library modules (`math`, `io`, `time`, `log`) are importable in Iron programs and their functions produce correct results
  4. Error messages display Rust-style diagnostics: source snippet, arrow pointing to the problem, and a suggestion; terminal output is colored; `--verbose` shows generated C
  5. The CI build runs with ASan+UBSan enabled and all tests pass clean (zero sanitizer errors)
**Plans:** 8/8 plans complete

Plans:
- [x] 03-01-PLAN.md
- [x] 03-02-PLAN.md
- [x] 03-03-PLAN.md
- [x] 03-04-PLAN.md
- [x] 03-05-PLAN.md
- [x] 03-06-PLAN.md
- [x] 03-07-PLAN.md
- [x] 03-08-PLAN.md

### Phase 4: Comptime, Game Dev, and Cross-Platform
**Goal**: Comptime evaluation works, raylib programs build and run, and the full toolchain produces binaries on macOS, Linux, and Windows
**Depends on**: Phase 3
**Requirements**: CT-01, CT-02, CT-03, CT-04, GAME-01, GAME-02, GAME-03, GAME-04
**Success Criteria** (what must be TRUE):
  1. A `comptime` expression evaluates at compile time and emits the result as a literal in the generated C; a step-limit violation produces a compile error rather than hanging
  2. `comptime read_file("assets/shader.glsl")` embeds the file contents as a string literal at compile time
  3. An Iron program that `import raylib` and uses the `draw {}` block compiles to a standalone binary that opens a window and handles input
  4. `iron build`, `iron run`, `iron check`, `iron fmt`, and `iron test` all produce correct results on macOS, Linux, and Windows
**Plans:** 6/6 plans complete

Plans:
- [x] 04-01-PLAN.md
- [x] 04-02-PLAN.md
- [x] 04-03-PLAN.md
- [x] 04-04-PLAN.md
- [x] 04-05-PLAN.md
- [x] 04-06-PLAN.md

### Phase 5: Codegen Fixes + Stdlib Wiring
**Goal**: String interpolation and parallel-for produce correct output; stdlib modules are callable from Iron source via import
**Depends on**: Phase 4
**Requirements**: GEN-01, GEN-11, STD-01, STD-02, STD-03, STD-04
**Success Criteria** (what must be TRUE):
  1. `"value is {x}"` where x=42 produces the string "value is 42" at runtime
  2. `parallel for i in range(100) { array[i] = i }` correctly distributes the range across pool workers
  3. `import math` followed by `val s = sin(1.0)` compiles and produces the correct result
  4. `import io` followed by `val content, err = read_file("test.txt")` compiles and returns file contents
  5. `import time` and `import log` modules are callable from Iron source
**Plans:** 5/5 plans complete

Plans:
- [x] 05-01-PLAN.md
- [x] 05-02-PLAN.md
- [x] 05-03-PLAN.md
- [x] 05-04-PLAN.md
- [x] 05-05-PLAN.md

### Phase 6: Milestone Gap Closure
**Goal**: Close the 3 remaining v1.0 audit gaps so all 52 requirements are fully satisfied
**Depends on**: Phase 5
**Requirements**: RT-07, STD-03, CLI-03
**Success Criteria** (what must be TRUE):
  1. `range(10)` is a recognized builtin callable from Iron source
  2. `Timer.create()`, `Timer.since()`, `Timer.reset()` are callable from Iron source via time.iron wrapper
  3. `iron check file_with_import_math.iron` succeeds
**Plans:** 2/2 plans complete

Plans:
- [x] 06-01-PLAN.md
- [x] 06-02-PLAN.md

</details>

<details>
<summary>v0.0.2-alpha Phase Details (Phases 7-11)</summary>

### Phase 7: IR Foundation
**Goal**: The complete IR data structure scaffold exists -- modules, functions, basic blocks, and every instruction kind -- with tooling to print and verify IR before any lowering code is written
**Depends on**: Phase 6
**Requirements**: IRCORE-01, IRCORE-02, IRCORE-03, IRCORE-04, TOOL-01, TOOL-02
**Success Criteria** (what must be TRUE):
  1. An IrModule can be constructed in a unit test containing functions, basic blocks, and instructions of every kind; all nodes are arena-allocated from a dedicated ir_arena separate from the AST arena
  2. Every IR instruction carries an Iron_Span and uses integer IrValueId operand references (not pointers); the IR type system reuses Iron_Type* directly with no IR-specific type wrappers
  3. `ir_print()` on a hand-built IrModule produces a human-readable text dump showing functions, blocks, instructions with value IDs, types, and source locations
  4. `ir_verify()` on a well-formed module passes; a module with a use-before-def, a missing block terminator, or an invalid branch target each produce a specific verification error
**Plans:** 2/2 plans complete

Plans:
- [x] 07-01-PLAN.md
- [x] 07-02-PLAN.md

### Phase 8: AST-to-IR Lowering
**Goal**: Every Iron language feature lowers from the annotated AST to typed SSA-form IR, producing a complete IrModule that passes verification for any valid Iron program
**Depends on**: Phase 7
**Requirements**: INSTR-01, INSTR-02, INSTR-03, INSTR-04, INSTR-05, INSTR-06, INSTR-07, INSTR-08, INSTR-09, INSTR-10, INSTR-11, INSTR-12, INSTR-13, CTRL-01, CTRL-02, CTRL-03, CTRL-04, MEM-01, MEM-02, MEM-03, MEM-04, CONC-01, CONC-02, CONC-03, CONC-04, MOD-01, MOD-02, MOD-03, MOD-04
**Success Criteria** (what must be TRUE):
  1. A straight-line Iron program using variables, arithmetic, comparisons, function calls, object construction, array literals, field access, and index access lowers to an IrModule that passes ir_verify; the printed IR shows correct SSA value numbering with alloca/load/store for mutable bindings
  2. An Iron program with if/else, while loops, match expressions, and early returns lowers to basic blocks connected by conditional branches, jumps, switch terminators, and return terminators; ir_verify confirms every block has exactly one terminator and all branch targets are valid
  3. An Iron program using heap allocation (auto_free and explicit), rc, defer with multiple exit points, and explicit free lowers correctly; defer is eagerly inlined at every exit point in the IR (no deferred IR instruction exists); auto_free produces explicit IrFree instructions
  4. An Iron program using lambdas, spawn, await, and parallel-for lowers correctly; lambdas and spawn bodies are lifted to top-level IrFunctions with environment structs; parallel-for chunk functions are lifted with captured variables
  5. Module-level type declarations (objects, enums, interfaces) produce correct vtable ordering; extern functions declare with C-level names; generic types are deduplicated through the monomorphization registry; IRON_NODE_DRAW removed from parser (draw becomes raylib.draw lambda)
**Plans:** 3/3 plans complete

Plans:
- [x] 08-01-PLAN.md
- [x] 08-02-PLAN.md
- [x] 08-03-PLAN.md

### Phase 9: C Emission and Cutover
**Goal**: The IR-to-C emission backend replaces the old AST-to-C codegen, producing identical program behavior verified by all existing integration tests
**Depends on**: Phase 8
**Requirements**: EMIT-01, EMIT-02, EMIT-03
**Success Criteria** (what must be TRUE):
  1. `iron build` compiles any valid Iron program through the AST-to-IR-to-C pipeline; the emitted C compiles with `clang -std=c11 -Wall -Werror` with zero warnings
  2. All existing integration tests pass through the new IR pipeline with identical runtime behavior (same stdout, same exit codes)
  3. The old `src/codegen/` AST-to-C path is fully removed from the codebase; no dual-path maintenance burden remains; the compiler has exactly one codegen path (IR-to-C)
**Plans:** 4/4 plans complete

Plans:
- [x] 09-01-PLAN.md
- [x] 09-02-PLAN.md
- [x] 09-03-PLAN.md
- [x] 09-04-PLAN.md

### Phase 10: Test Hardening
**Goal**: Reorganize the test directory, add real algorithm tests and comprehensive IR coverage tests so that the new IR pipeline is proven correct across a wide range of programs -- from isolated instruction-level tests to real-world composite programs
**Depends on**: Phase 9
**Requirements**: THARD-01, THARD-02, THARD-03, THARD-04, THARD-05, THARD-06, THARD-07, THARD-08, THARD-09, THARD-10
**Success Criteria** (what must be TRUE):
  1. Test directory has clear structure (unit/, integration/, algorithms/, ir/) with consistent naming; all existing tests migrated without breakage; CMake/CI updated
  2. Real algorithm tests (quicksort, subset sum, binary search, fibonacci, merge sort, BFS/DFS) compile through the IR pipeline and produce correct output
  3. Every IR instruction kind has a dedicated lowering unit test with a minimal .iron snippet; every verifier invariant has a negative test that triggers the specific error
  4. Edge case tests cover deeply nested control flow, multiple early returns with defer, heap auto_free in loops, rc patterns, lambda capture of mutables, nested spawn/await, and generic stress patterns
  5. Real-world composite programs (mini game loop, CLI file processor, concurrent pipeline) compile and run correctly; IR printer snapshot tests catch regressions in IR output
**Plans:** 5/5 plans complete

Plans:
- [x] 10-01-PLAN.md
- [x] 10-02-PLAN.md
- [x] 10-03-PLAN.md
- [x] 10-04-PLAN.md
- [x] 10-05-PLAN.md

### Phase 11: Release Pipeline & Versioning
**Goal**: CI automatically builds downloadable release binaries for all platforms when a draft GitHub release is created, and `iron --version` outputs version, commit hash, and build date in rustc style
**Depends on**: Phase 10
**Requirements**: REL-01, REL-02, REL-03, REL-04
**Success Criteria** (what must be TRUE):
  1. Creating a draft release on GitHub triggers a CI workflow that builds iron binaries for macOS (arm64 + x86_64), Linux (x86_64), and Windows (x86_64)
  2. Built binaries are uploaded as release assets and are directly downloadable and executable on each platform
  3. `iron --version` outputs `iron X.Y.Z (abcdef0 2026-MM-DD)` matching the rustc format with the actual commit hash and build date
  4. Commit hash and build date are baked in at compile time via CMake defines, not runtime detection
**Plans:** 2/2 plans complete

Plans:
- [x] 11-01-PLAN.md
- [x] 11-02-PLAN.md

</details>

<details>
<summary>v0.0.3-alpha Phase Details (Phases 12-14)</summary>

### Phase 12: Binary Split and Installation
**Goal**: The toolchain consists of two separate installed binaries (`ironc` for raw compilation, `iron` for project management), discoverable at runtime, with all existing single-file workflows preserved
**Depends on**: Phase 11
**Requirements**: CLI2-01, CLI2-02, CLI2-03, CLI2-04
**Success Criteria** (what must be TRUE):
  1. `ironc build main.iron`, `ironc run main.iron`, `ironc check main.iron`, `ironc fmt main.iron`, and `ironc test tests/` all behave identically to the old `iron` equivalents -- no behavior change, only a binary rename
  2. `iron build main.iron` continues to work and produce the same output as before (backward compat fallback); a file argument triggers single-file mode with an optional deprecation hint
  3. The `iron` binary discovers `ironc` at runtime by resolving its own executable path and checking for a sibling binary -- no compile-time hardcoded paths, no `IRON_SOURCE_DIR` dependency
  4. `cmake --install` (or the install script) installs both `iron` and `ironc` to the same directory and adds that directory to PATH
**Plans:** 3/3 plans complete

Plans:
- [x] 12-01-PLAN.md
- [x] 12-02-PLAN.md
- [x] 12-03-PLAN.md

### Phase 13: Project Workflow
**Goal**: Users can create, build, run, check, and test iron.toml projects using the `iron` CLI with Cargo-style colored output
**Depends on**: Phase 12
**Requirements**: PROJ-01, PROJ-02, PROJ-03, PROJ-04, PROJ-05, PROJ-06, DEP-01, DEP-02, CLI2-05
**Success Criteria** (what must be TRUE):
  1. `iron init` in an empty directory creates `iron.toml` (with `[package]` and empty `[dependencies]`), `src/main.iron`, and `.gitignore`; `iron init --lib` creates `src/lib.iron` instead
  2. `iron build` in a directory with `iron.toml` reads the manifest, collects source files, and produces a working binary via `ironc` -- no dependency section required
  3. `iron run` builds and immediately executes the package binary; `iron check` reports type errors without producing any output file; `iron test` discovers and runs all tests in the package
  4. All `iron` status lines use Cargo-style formatting: `  Compiling name v0.1.0`, `   Finished dev in 0.42s`, `    Running ./target/name`; colors are suppressed when stdout is not a TTY or `NO_COLOR` is set; Windows enables VT processing at startup
  5. `iron.toml` accepts `[dependencies]` entries in the form `name = { git = "owner/repo", version = "X.Y.Z" }` and parses them without error (resolution deferred to Phase 14)
**Plans:** 2/2 plans complete

Plans:
- [x] 13-01-PLAN.md
- [x] 13-02-PLAN.md

### Phase 14: Dependency Resolution and Lockfile
**Goal**: Iron projects can declare GitHub-sourced dependencies that are automatically fetched, compiled together, and pinned in a reproducible lockfile
**Depends on**: Phase 13
**Requirements**: DEP-03, DEP-04, DEP-05
**Success Criteria** (what must be TRUE):
  1. `iron build` in a project with a `[dependencies]` entry downloads the dependency source from GitHub into `~/.iron/cache/` and includes it in the build; a second `iron build` uses the cache without re-downloading
  2. After the first successful build, `iron.lock` exists in the project directory with `lock_version = 1` as its first field, one `[[package]]` entry per dependency, and a 40-character commit SHA (never a mutable tag name) for each entry
  3. A project that depends on a library which itself has dependencies resolves and builds all transitive dependencies; a circular dependency (A depends on B depends on A) produces a clear error and exits rather than looping
  4. Deleting `~/.iron/cache/` and running `iron build` on a project with an existing `iron.lock` re-fetches the exact same commit SHAs recorded in the lockfile, producing bit-identical dependency source
**Plans:** 2/2 plans complete

Plans:
- [x] 14-01-PLAN.md
- [x] 14-02-PLAN.md

</details>

<details>
<summary>v0.0.5-alpha Phase Details (Phases 15-20)</summary>

### Phase 15: Copy Propagation, DCE & Constant Folding
**Goal**: The compiler eliminates trivial copies, dead instructions, and compile-time constant expressions from IR before C emission, producing cleaner generated code
**Depends on**: Phase 14
**Requirements**: IROPT-01, IROPT-03, IROPT-04, INFRA-02
**Success Criteria** (what must be TRUE):
  1. A new `ir_optimize.c` module exists and is called between phi elimination and C emission; the optimization pipeline is invocable on any IrModule and all 127 benchmarks plus 42+ integration tests still pass after optimization
  2. Copy propagation replaces all uses of a trivially copied value with the original; an IR dump of `val x = y` shows the copy eliminated and downstream uses referencing y directly
  3. Dead code elimination removes instructions whose results are never referenced by any other instruction or terminator; an IR dump of a function with an unused arithmetic result shows the instruction absent
  4. Constant folding evaluates `3 + 4` to `7` at compile time and propagates constants through store/load chains; the emitted C contains the literal `7` rather than `3 + 4`
**Plans:** 3/3 plans complete

Plans:
- [x] 15-01-PLAN.md
- [x] 15-02-PLAN.md
- [x] 15-03-PLAN.md

### Phase 16: Expression Inlining
**Goal**: The C emission backend reconstructs compound expressions for single-use pure values instead of emitting one temporary variable per IR instruction, closing the largest single performance gap
**Depends on**: Phase 15
**Requirements**: IROPT-02
**Success Criteria** (what must be TRUE):
  1. A single-use pure IR value (arithmetic, comparison, field access) is inlined into its use site during C emission rather than assigned to a temporary variable; `int t1 = a + b; int t2 = t1 * c;` becomes `int t2 = (a + b) * c;`
  2. Values with multiple uses, side effects (calls, stores), or that cross basic block boundaries are NOT inlined and remain as named temporaries
  3. The median_two_sorted_arrays benchmark shows measurable improvement toward the 1.2x target (was 4.5x before optimization passes)
**Plans:** 3/3 plans complete

Plans:
- [x] 16-01-PLAN.md
- [x] 16-02-PLAN.md
- [x] 16-03-PLAN.md

### Phase 17: Strength Reduction & Store/Load Elimination
**Goal**: Loop index patterns use induction variables instead of multiply-add, and redundant memory operations are removed
**Depends on**: Phase 16
**Requirements**: IROPT-05, IROPT-06
**Success Criteria** (what must be TRUE):
  1. A loop containing `i * cols + j` for array indexing is transformed to use an induction variable that increments by `cols` each outer iteration; the emitted C shows addition instead of multiplication inside the inner loop
  2. A STORE immediately followed by a LOAD of the same address with no intervening modification is reduced to a single STORE with the loaded value replaced by the stored value; the IR dump confirms the redundant LOAD is removed
  3. All 127 benchmarks and integration tests continue to pass after both passes are applied; no correctness regressions
**Plans:** 3/3 plans complete

Plans:
- [x] 17-01-PLAN.md
- [x] 17-02-PLAN.md
- [x] 17-03-PLAN.md

### Phase 18: Benchmark Validation
**Goal**: Every benchmark meets its configured performance threshold, confirming the optimization passes collectively close the performance gap
**Depends on**: Phase 17
**Requirements**: BENCH-01, BENCH-02, BENCH-03, BENCH-04, BENCH-05
**Success Criteria** (what must be TRUE):
  1. median_two_sorted_arrays runs at 1.2x C parity or better (down from 4.5x)
  2. count_paths_with_obstacles, min_path_sum, largest_rect_histogram, max_depth_binary_tree, target_sum, three_sum, and num_islands all run at 1.2x C parity or better
  3. All 127 benchmarks pass at their configured parity thresholds with 100% pass rate; `iron bench` or the benchmark runner reports zero failures
  4. Any benchmark that still exceeds 1.2x is investigated, root-caused, and either fixed with a targeted optimization or its threshold is justified and documented
**Plans:** 4/4 plans complete

Plans:
- [x] 18-01-PLAN.md
- [x] 18-02-PLAN.md
- [x] 18-03-PLAN.md
- [x] 18-04-PLAN.md

### Phase 19: LIR Rename & HIR Foundation
**Goal**: The current IR is renamed to Lower IR (LIR) throughout the codebase, and a new High IR layer with structured control flow, named variables, and language-level constructs is established with printer and verifier
**Depends on**: Phase 18
**Requirements**: INFRA-01, HIR-01, HIR-02, HIR-03, HIR-04, HIR-07, HIR-08
**Success Criteria** (what must be TRUE):
  1. All IR types, functions, files, and comments are renamed from Ir/ir_ to Lir/lir_ (Lower IR); the codebase compiles cleanly and all tests pass with the new naming
  2. HIR data structures represent structured control flow (if/else, for, while, match) as tree nodes rather than CFG basic blocks; let/mut bindings carry names and lexical scope information
  3. HIR represents method calls with receivers, field access, array indexing, closures, spawn, parallel-for, and defer as first-class language-level constructs (not lowered to primitives)
  4. `hir_print()` outputs a human-readable representation that resembles Iron source code; `hir_verify()` validates scope nesting, type consistency, and structural completeness
**Plans:** 3/3 plans complete

Plans:
- [x] 19-01-PLAN.md
- [x] 19-02-PLAN.md
- [x] 19-03-PLAN.md

### Phase 20: HIR Lowering & Pipeline Cutover
**Goal**: The compiler pipeline is AST-to-HIR-to-LIR-to-C, with the direct AST-to-LIR path removed after parity is confirmed
**Depends on**: Phase 19
**Requirements**: HIR-05, HIR-06, INFRA-03, INFRA-04
**Success Criteria** (what must be TRUE):
  1. AST-to-HIR lowering covers all Iron language features and produces a valid HIR module that passes hir_verify for any valid Iron program
  2. HIR-to-LIR lowering produces SSA-form output equivalent to the current AST-to-LIR path; structured control flow is flattened to basic blocks with branches, named variables become alloca/load/store sequences
  3. All 127 benchmarks and 42+ integration tests pass through the full AST-to-HIR-to-LIR-to-C pipeline with identical behavior to the current path
  4. The old direct AST-to-LIR lowering path is removed from the codebase; the compiler has exactly one lowering pipeline (AST-to-HIR-to-LIR-to-C)
**Plans:** 7/7 plans complete

Plans:
- [x] 20-01-PLAN.md
- [x] 20-02-PLAN.md
- [x] 20-03-PLAN.md
- [x] 20-04-PLAN.md
- [x] 20-05-PLAN.md
- [x] 20-06-PLAN.md
- [x] 20-07-PLAN.md

</details>

<details>
<summary>v0.0.6-alpha Phase Details (Phases 21-23)</summary>

### Phase 21: Parallel-For Fix
**Goal**: The HIR pipeline correctly emits `__pfor_` helper functions so all parallel-for integration tests and pfor-dependent algorithm tests compile and pass
**Depends on**: Phase 20
**Requirements**: PFOR-01, PFOR-02, PFOR-03
**Success Criteria** (what must be TRUE):
  1. `is_lifted_func()` in `emit_c.c` recognizes the `__pfor_` prefix and routes pfor helper functions to the correct output section; the fix is a single predicate change, not a structural change
  2. The three pfor integration tests (test_parallel, parallel_for_capture, hir_parallel_for) all compile and produce correct output
  3. The four pfor-dependent algorithm tests (concurrent_hash_map, graph_bfs_dfs, parallel_merge_sort, work_stealing) compile and pass
**Plans:** 1/1 plans complete

Plans:
- [x] 21-01-PLAN.md

### Phase 22: Struct Codegen Fix
**Goal**: Struct values are passed cleanly through the HIR pipeline -- no spurious constructor wrapping, no excess CONSTRUCT field emission -- and all struct-dependent algorithm tests produce correct output
**Depends on**: Phase 21
**Requirements**: STRUCT-01, STRUCT-02, STRUCT-03, ALG-01, ALG-02, ALG-03
**Success Criteria** (what must be TRUE):
  1. `hir_to_lir.c` only lowers a function call as a constructor when the function name matches the struct type name; calling a function that returns a struct no longer produces spurious `{ .frame = _v32 }` wrapping when `_v32` is already the full struct
  2. CONSTRUCT emission in `emit_c.c` emits exactly `od->field_count` elements, never more; no excess field values appear in the generated C initializer list
  3. The game_loop_headless composite test passes with correct struct value passing end-to-end
  4. quicksort produces a correctly sorted output array at runtime
  5. hash_map runs without crash and produces correct key-value lookup results
**Plans:** 2/2 plans complete

Plans:
- [x] 22-01-PLAN.md
- [x] 22-02-PLAN.md

### Phase 23: Correctness Audit
**Goal**: A systematic review of the HIR-to-LIR-to-C pipeline surface area identifies any remaining correctness bugs, all found issues are fixed with regression tests, and new test cases cover the problem areas addressed this milestone
**Depends on**: Phase 22
**Requirements**: AUDIT-01, AUDIT-02, AUDIT-03
**Success Criteria** (what must be TRUE):
  1. Every HIR instruction kind and every emit_c.c emission path has been reviewed for correctness against the Iron language spec; the audit produces a written checklist of what was checked and what (if anything) was found
  2. Any correctness bug found during the audit is fixed with a targeted code change and a regression test that would have caught the original bug
  3. New .iron test cases cover parallel-for emission, struct value passing, and any other problem areas uncovered in phases 21-22 or the audit; the test suite grows by at least 5 new correctness-focused tests
**Plans:** 2/2 plans complete

Plans:
- [x] 23-01-PLAN.md
- [x] 23-02-PLAN.md

</details>

<details>
<summary>v0.0.7-alpha Phase Details (Phases 24-31)</summary>

### Phase 24: Range Bound Hoisting
**Goal**: Every for-range loop evaluates its bound exactly once in the pre-header block instead of on every back-edge, eliminating redundant `Iron_range()` calls from the hot path
**Depends on**: Phase 23
**Requirements**: LOOP-01, LOOP-02
**Success Criteria** (what must be TRUE):
  1. Generated C for `for i in range(n)` stores the upper bound in a pre-header local; the while-loop condition compares against that local, not a fresh `Iron_range()` call
  2. All 137 benchmarks and all integration tests pass with no correctness regressions after the change
  3. For-range-heavy benchmarks (e.g., quicksort, binary_search, loop_patterns) show measurable runtime reduction compared to the pre-hoisting baseline
**Plans:** 1/1 plans complete

Plans:
- [x] 24-01-PLAN.md

### Phase 25: Stack Array Promotion
**Goal**: `fill(CONST, val)` calls where the count is a compile-time constant <=1024 and the result does not escape the function emit stack-allocated arrays via `alloca()` instead of heap allocation
**Depends on**: Phase 24
**Requirements**: MEM-01, MEM-02
**Success Criteria** (what must be TRUE):
  1. Generated C for `var parent = fill(50, 0)` in a non-escaping function emits an `alloca()`-based declaration at function entry rather than a heap allocation call
  2. The array declaration appears at function entry (not at the call site), so no VLA+goto bypass error occurs when the function contains control flow
  3. All 137 benchmarks and integration tests pass; `bug_vla_goto_bypass.iron` continues to pass
**Plans:** 1/1 plans complete

Plans:
- [x] 25-01-PLAN.md

### Phase 26: LOAD Expression Inlining
**Goal**: LOAD instructions are eligible for expression inlining when their single use is in the same basic block as the LOAD definition, removing one layer of copy temporaries from every function
**Depends on**: Phase 25
**Requirements**: EXPR-01, EXPR-02
**Success Criteria** (what must be TRUE):
  1. A LOAD whose only use is in the same block as its definition is inlined directly at the use site; the emitted C omits the intermediate `_vN` variable for that LOAD
  2. A LOAD whose use is in a different block from its definition is NOT inlined; the intermediate variable remains and no undeclared-variable C errors occur
  3. All 137 benchmarks and integration tests pass; no new undeclared-variable errors appear in the generated C
**Plans:** 1/1 plans complete

Plans:
- [x] 26-01-PLAN.md

### Phase 27: Function Inlining
**Goal**: Small, pure, non-recursive functions are inlined at LIR level before the copy-prop/DCE fixpoint so the merged code receives full optimizer benefit
**Depends on**: Phase 26
**Requirements**: INLINE-01, INLINE-02, INLINE-03
**Success Criteria** (what must be TRUE):
  1. Functions with <=20 LIR instructions that are non-recursive and have no side effects are replaced at their call sites with a copy of the callee body; the callee function is removed from the module if it has no remaining callers
  2. Every cloned instruction uses a fresh ValueId remapped via a callee-to-caller ID table; no existing caller value_table entries are overwritten
  3. The inlining pass runs before the copy-prop/DCE fixpoint; inlined code is subsequently optimized by copy propagation and dead-code elimination in the same fixpoint run
  4. All 137 benchmarks and integration tests pass; call-heavy benchmarks (e.g., connected_components) show measurable runtime reduction
**Plans:** 2/2 plans complete

Plans:
- [x] 27-01-PLAN.md

### Phase 28: Phi Elimination Improvement
**Goal**: Dead alloca elimination removes write-only phi-origin temporaries from generated C, reducing variable count in functions with complex control flow
**Depends on**: Phase 27
**Requirements**: PHI-01, PHI-02
**Success Criteria** (what must be TRUE):
  1. Phi elimination uses copy coalescing to merge phi-related copies where lifetimes permit; the number of distinct temporary variables emitted for a function with many phi nodes is measurably lower than before
  2. The connected_components benchmark shows a measurable reduction in generated temporaries compared to the Phase 27 baseline
  3. All 137 benchmarks and integration tests pass with no correctness regressions
**Plans:** 1/1 plans complete

Plans:
- [x] 28-01-PLAN.md

### Phase 29: Sized Integers
**Goal**: Iron programs can declare `Int32` variables that emit `int32_t` in generated C, enabling 32-bit memory bandwidth for array-heavy algorithms
**Depends on**: Phase 28
**Requirements**: INT-01, INT-02
**Success Criteria** (what must be TRUE):
  1. `val x: Int32 = 0` compiles through the full pipeline and emits `int32_t x = 0;` in the generated C; the Iron type system accepts `Int32` in variable declarations, function parameters, and return types
  2. An array declared as `[Int32]` emits a `int32_t*` pointer in generated C; arithmetic on `Int32` elements operates on 32-bit values at runtime
  3. All existing tests continue to pass; `Int32` programs compile with `clang -std=c11 -Wall -Werror` with zero warnings
**Plans:** 2/2 plans complete

Plans:
- [x] 29-01-PLAN.md
- [x] 29-02-PLAN.md

### Phase 30: Benchmark Validation and Exploration
**Goal**: The full benchmark suite is compared against the pre-optimization baseline to confirm measurable improvement; any remaining high-overhead benchmarks are analyzed for additional optimization opportunities
**Depends on**: Phase 29
**Requirements**: BENCH-01, BENCH-02
**Success Criteria** (what must be TRUE):
  1. A benchmark comparison run with `--compare` mode shows aggregate improvement over the v0.0.6-alpha baseline; the majority of benchmarks that previously exceeded 3x of C now meet or beat that threshold
  2. The connected_components benchmark shows measurable improvement over its v0.0.6-alpha ratio (was ~167x); the remaining gap is identified and attributed to known out-of-scope factors (goto-based loops, int64 width)
  3. Any benchmark still above 3x that is not explained by an out-of-scope factor is documented with a root-cause analysis and a concrete proposal for a follow-up optimization
**Plans:** 2/2 plans complete

Plans:
- [x] 30-01-PLAN.md
- [x] 30-02-PLAN.md

### Phase 31: Spawn/Await Correctness
**Goal:** Make `await handle` work end-to-end so spawned tasks can be joined and their return values retrieved; fix concurrency benchmarks to use await for fair timing comparison against C's pthread_join; add compiler warning for un-awaited spawn handles
**Requirements**: SPAWN-01, SPAWN-02, SPAWN-03, SPAWN-04, SPAWN-05, BENCH-01, BENCH-02
**Depends on:** Phase 30
**Success Criteria** (what must be TRUE):
  1. `val h = spawn("name") { return 42 }; val r = await h` compiles and r == 42
  2. Multiple spawn+await pairs work correctly in sequence
  3. Fire-and-forget spawn compiles with compiler warning about un-captured handle
  4. parallel-for blocks until all iterations complete (no regression)
  5. spawn_independent_work benchmark uses await for fair timing vs C pthread_join
  6. Full benchmark and test suites pass with updated thresholds
**Plans:** 2/2 plans complete

Plans:
- [x] 31-01-PLAN.md
- [x] 31-02-PLAN.md

</details>

<details>
<summary>v0.0.8-alpha Phase Details (Phases 32-39)</summary>

### Phase 32: LIR Verifier Hardening
**Goal**: The LIR verifier catches type mismatches in PHI nodes and call instructions before they reach the C emitter, preventing invalid C output from well-typed Iron programs
**Depends on**: Phase 31
**Requirements**: LIR-01, LIR-02, LIR-03, LIR-04, LIR-05
**Success Criteria** (what must be TRUE):
  1. A PHI node whose incoming values have different types from the PHI result type produces `IRON_ERR_LIR_PHI_TYPE_MISMATCH` at verification time, not a silent C compilation failure
  2. A direct call instruction whose argument types do not match the callee's parameter types produces `IRON_ERR_LIR_CALL_TYPE_MISMATCH` at verification time
  3. A direct call instruction with wrong argument count produces a diagnostic at verification time
  4. All existing tests and benchmarks continue to pass (no false positives on valid code)
**Plans**: 2/2 plans complete

Plans:
- [x] 32-01-PLAN.md
- [x] 32-02-PLAN.md

### Phase 33: Type Validation Checks
**Goal**: The type checker catches match exhaustiveness violations, unsafe casts, non-stringable interpolations, and narrow-integer overflow risks at compile time
**Depends on**: Phase 32
**Requirements**: MATCH-01, MATCH-02, MATCH-03, CAST-01, CAST-02, CAST-03, CAST-04, STRN-01, STRN-02, OVFL-01, OVFL-02, OVFL-03
**Success Criteria** (what must be TRUE):
  1. A match statement on an enum type that does not cover all variants and has no else clause produces `IRON_ERR_NONEXHAUSTIVE_MATCH` listing the uncovered variants; a match on a non-enum type without else also errors
  2. A cast from a non-numeric, non-bool type (e.g., String to Int) produces `IRON_ERR_INVALID_CAST`; a wider-to-narrower integer cast produces `IRON_WARN_NARROWING_CAST`; a constant value outside the target range produces a specific error
  3. A string interpolation containing a type that is not a primitive, String, Bool, or has no `to_string` method produces `IRON_ERR_NOT_STRINGABLE`
  4. A compound assignment (`+=`, `-=`, `*=`) on a narrow integer type (Int8, Int16, UInt8, etc.) where the RHS is not a fitting constant produces `IRON_WARN_POSSIBLE_OVERFLOW`
  5. All existing tests and benchmarks continue to pass (no false positives on valid code)
**Plans**: 3/3 plans complete

Plans:
- [x] 33-01-PLAN.md
- [x] 33-02-PLAN.md
- [x] 33-03-PLAN.md

### Phase 34: Bounds Checking
**Goal**: Constant array indices and slice bounds that are provably out of range produce compile-time errors instead of generating C that crashes at runtime
**Depends on**: Phase 33
**Requirements**: BOUNDS-01, BOUNDS-02, BOUNDS-03, SLICE-01, SLICE-02, SLICE-03, SLICE-04
**Success Criteria** (what must be TRUE):
  1. `arr[5]` on an array of known size 3 produces `IRON_ERR_INDEX_OUT_OF_BOUNDS`; `arr[-1]` also errors; `arr[2]` on size-3 array passes without diagnostic
  2. An array index expression that is not an integer type produces a type error diagnostic
  3. Slice expressions where start > end (both constants) produce `IRON_ERR_INVALID_SLICE_BOUNDS`; slices where bounds exceed array size (all constants) also error
  4. Slice start and end expressions that are not integer types produce a type error diagnostic
  5. All existing tests and benchmarks continue to pass (no false positives on valid code)
**Plans**: 2/2 plans complete

Plans:
- [x] 34-01-PLAN.md
- [x] 34-02-PLAN.md

### Phase 35: Escape Analysis Extension
**Goal**: Escape analysis tracks heap values through field assignments, array index assignments, and function argument passing so that escape-dependent optimizations and warnings cover real-world patterns
**Depends on**: Phase 34
**Requirements**: ESC-01, ESC-02, ESC-03, ESC-04
**Success Criteria** (what must be TRUE):
  1. `obj.field = heap_val` correctly marks `heap_val` as escaping; the compiler does not insert a premature free for values stored into object fields
  2. `arr[i] = heap_val` correctly marks `heap_val` as escaping; the compiler does not insert a premature free for values stored into array elements
  3. Passing a heap value as a function argument marks it as escaping; no premature free is inserted for values passed to functions
  4. `expr_ident_name()` or equivalent correctly resolves field-access and index-access targets for escape tracking
  5. All existing tests and benchmarks continue to pass (no false positives on valid code)
**Plans**: 1/1 plans complete

Plans:
- [x] 35-01-PLAN.md

### Phase 36: Definite Assignment Analysis
**Goal**: Variables that may be read before being assigned on all control flow paths produce a compile-time error, preventing undefined behavior from uninitialized reads
**Depends on**: Phase 35
**Requirements**: INIT-01, INIT-02, INIT-03, INIT-04
**Success Criteria** (what must be TRUE):
  1. A variable declared without an initializer and used before any assignment on all paths produces `IRON_ERR_POSSIBLY_UNINITIALIZED`
  2. A variable assigned in one branch of an if/else but not the other, then used after the if/else, produces `IRON_ERR_POSSIBLY_UNINITIALIZED`
  3. A variable assigned inside a loop body but used after the loop (which may execute zero times) produces `IRON_ERR_POSSIBLY_UNINITIALIZED`
  4. A variable assigned in all branches of a match/if-else and used afterwards does NOT produce a false positive
  5. All existing tests and benchmarks continue to pass (analysis uses bounded worklist, no excessive memory)
**Plans**: 2/2 plans complete

Plans:
- [x] 36-01-PLAN.md
- [x] 36-02-PLAN.md

### Phase 37: Generic Constraint Checking
**Goal**: Concrete type arguments that do not satisfy declared generic constraints are rejected at instantiation sites with a clear diagnostic, preventing downstream type errors in monomorphized code
**Depends on**: Phase 36
**Requirements**: GEN-01, GEN-02, GEN-03, GEN-04
**Success Criteria** (what must be TRUE):
  1. `fn sort<T: Comparable>(arr: [T])` called with a type that does not implement `Comparable` produces `IRON_ERR_CONSTRAINT_NOT_SATISFIED` naming the constraint and the failing type
  2. `var s = Stack<MyType>()` where `Stack` requires `T: Printable` and `MyType` does not implement `Printable` produces the same diagnostic at the construction site
  3. A generic call or construction with a type that satisfies the constraint compiles without diagnostic
  4. All existing tests and benchmarks continue to pass (no false positives on valid code)
**Plans**: 2/2 plans complete

Plans:
- [x] 37-01-PLAN.md
- [x] 37-02-PLAN.md

### Phase 38: Concurrency Safety
**Goal**: Mutation detection in parallel and spawn blocks covers field access, array index expressions, and read-write race patterns so that concurrent code with data races produces compile-time warnings
**Depends on**: Phase 37
**Requirements**: CONC-01, CONC-02, CONC-03, CONC-04, CONC-05
**Success Criteria** (what must be TRUE):
  1. `parallel for ... { obj.field = val }` where `obj` is a shared outer variable produces a mutation warning (not just bare `obj = val`)
  2. `parallel for ... { arr[i] = val }` where `arr` is a shared outer variable produces a mutation warning for non-partitioned access patterns
  3. A spawn block that reads a variable also written by the outer scope (or another spawn) produces a read-write race diagnostic
  4. Spawn blocks perform capture analysis identifying which outer variables are referenced; mutable captures are flagged as potential data races
  5. All existing tests and benchmarks continue to pass (no false positives on valid concurrent patterns like partitioned parallel-for)
**Plans**: 2/2 plans complete

Plans:
- [x] 38-01-PLAN.md
- [x] 38-02-PLAN.md

### Phase 39: Diagnostic Test Sweep
**Goal**: Every new diagnostic introduced in phases 32-38 has both positive tests (triggers the diagnostic) and negative tests (valid code that must not trigger it), with edge-case coverage for complex analyses
**Depends on**: Phase 38
**Requirements**: TEST-01, TEST-02, TEST-03
**Success Criteria** (what must be TRUE):
  1. Every `IRON_ERR_*` and `IRON_WARN_*` diagnostic added in this milestone has at least one .iron test file that triggers it and verifies the error code
  2. Every new diagnostic has at least one .iron test file with valid code that exercises the same language feature without triggering the diagnostic (no false positives)
  3. Definite assignment analysis has edge-case tests for nested if/else, match with fallthrough, loops with break/continue, and early returns
  4. Concurrency analysis has edge-case tests for nested spawn, spawn inside parallel-for, and multiple spawn blocks sharing variables
  5. The full test suite passes with zero regressions; test count grows by at least 30 new diagnostic-focused tests
**Plans**: 2/2 plans complete

Plans:
- [x] 39-01-PLAN.md
- [x] 39-02-PLAN.md

</details>

### v0.1-alpha Static Interface Dispatch Phase Details

### Phase 40: Syntax & Core Dispatch
**Goal**: Interface-typed variables compile correctly via tagged union construction and tag-based dispatch, with no vtable infrastructure remaining in the codebase
**Depends on**: Phase 39
**Requirements**: SYN-01, DISP-01, DISP-02, DISP-03, DISP-04, DISP-05
**Success Criteria** (what must be TRUE):
  1. `object Dog impl Animal` is accepted by the parser as equivalent to `object Dog implements Animal`; existing `implements` syntax continues to work; the `impl` keyword is a reserved token
  2. The compiler scans all source files and builds a complete `IfaceRegistry` mapping each interface to its concrete implementors with canonical tag assignments; tag numbers are stable across compilations (alphabetical order)
  3. `let s: Shape = Circle { radius: 5.0 }` compiles to a tagged union value (tag enum + payload union + outer struct); `s.area()` dispatches via match on the tag discriminant, not a vtable lookup; the generated C compiles and produces the correct result
  4. An implementor that is declared but never instantiated anywhere in the program is excluded from the generated tagged union and dispatch switches; the generated C contains no reference to the eliminated type
  5. The vtable struct generation block in `emit_c.c` is removed; no vtable-related C structs, typedefs, or function pointer tables appear in any generated output; all existing tests and benchmarks pass without regression
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 41: Collection Splitting
**Goal**: Interface-typed collections are internally represented as per-type homogeneous arrays with correct insertion routing and both ordered and unordered iteration strategies
**Depends on**: Phase 40
**Requirements**: COLL-01, COLL-02, COLL-03, COLL-04
**Success Criteria** (what must be TRUE):
  1. `var animals: List<Animal> = [Dog{}, Cat{}, Dog{}]` compiles to a split collection with separate `Dog[]` and `Cat[]` sub-arrays; adding an element routes it to the correct sub-array based on its concrete type tag
  2. A `for a in animals { a.speak() }` loop where the compiler proves order-independence (no index-dependent logic, no cross-iteration state) emits separate per-type loops without an order index array
  3. A `for a in animals { print(a) }` loop where insertion order is observed emits an order index array and iterates via tag+offset indirection to preserve the original insertion sequence
  4. `var shapes: List<Shape> unordered = [...]` parses the `unordered` annotation and forces unordered split iteration regardless of the compiler's order analysis; the annotation produces no warning when the compiler also selects unordered
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 42: Memory Layout
**Goal**: The compiler selects optimal memory layout per (collection, type) pair and eliminates waste from unused fields, shared fields, and oversized non-collection variants
**Depends on**: Phase 41
**Requirements**: LAYOUT-01, LAYOUT-02, LAYOUT-03, LAYOUT-04, LAYOUT-05
**Success Criteria** (what must be TRUE):
  1. A collection where all access is field-by-field in a loop (`for s in shapes { total += s.area }`) emits SoA layout (separate arrays per field) for that concrete type; a collection with whole-object access (`shapes[i]`) emits AoS layout; the selection is automatic with no annotation required
  2. A field that is declared on an implementor but never accessed through any interface operation on that collection is excluded from the collection storage struct; direct concrete-type access (`my_circle.debug_name`) still works because the primary struct retains all fields
  3. Fields with the same name, type, and position across all implementors of an interface are stored in a single shared array rather than duplicated in each per-type sub-array
  4. A non-collection interface variable where one implementor is significantly larger than others stores the large variant indirectly (pointer) while small variants are stored inline; the threshold is determined by the compiler
  5. `var shapes: List<Shape> layout: soa = [...]` forces SoA layout; `layout: aos` forces AoS layout; when the annotation contradicts the compiler's analysis, a warning is emitted explaining why the compiler would have chosen differently
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 43: Loop Fusion & Monomorphic Specialization
**Goal**: Chained collection operations execute in a single pass per type, and collections proven to hold only one concrete type collapse to plain typed arrays
**Depends on**: Phase 42
**Requirements**: OPT-01, OPT-02
**Success Criteria** (what must be TRUE):
  1. `shapes.filter(s => s.area > 10).map(s => s.area * 2).sum()` emits a single fused loop per concrete type that applies the filter predicate, the map transform, and the sum accumulation in one pass; no intermediate collection is allocated
  2. A function that receives `List<Shape>` where dataflow proves all elements are `Circle` (e.g., constructed locally with only `Circle` insertions) emits a plain `Circle[]` array with direct field access instead of tag-based dispatch; the split collection machinery is bypassed entirely
  3. Monomorphic specialization does not produce duplicate function bodies -- a specialization registry keyed on `(function_name, concrete_type)` prevents re-emission of the same specialized function
  4. All existing tests pass; chained collection operations produce identical results to the non-fused path
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 44: Hardware Acceleration & Value Optimization
**Goal**: Split collections use efficient memory allocation, hot loops receive hardware prefetch hints, independent per-type loops are parallelized, and fields are stored in the narrowest sufficient type
**Depends on**: Phase 43
**Requirements**: OPT-03, OPT-04, OPT-05, OPT-06
**Success Criteria** (what must be TRUE):
  1. Per-type sub-arrays in split collections use arena allocation with geometric growth (doubling); the arena is per-type so that each sub-array's elements are contiguous in memory; the generated C shows `arena_alloc` calls instead of individual `malloc` calls
  2. Hot SoA loops and ordered iteration index access emit `__builtin_prefetch` hints with per-field size-adjusted prefetch distances; the prefetch macro is portable across GCC, Clang, and MSVC (no-op on unsupported compilers)
  3. Per-type loops that are proven independent (no shared mutable state, no cross-type data dependencies) emit `#pragma omp parallel sections` wrapping each type's loop; the generated C compiles correctly both with and without `-fopenmp`
  4. A field proven by whole-program analysis to hold values fitting in a smaller type (e.g., an `Int` that only stores 0-255) is stored as `uint8_t` in collection storage structs; the compiler emits widening reads and narrowing writes at access sites
  5. All existing tests pass; programs compiled without `-fopenmp` ignore the OpenMP pragmas with no warnings or errors
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 45: Documentation & Branding
**Goal**: The language guide, compiler internals documentation, and website reflect the static interface dispatch capability with clear examples and updated branding
**Depends on**: Phase 44
**Requirements**: DOCS-01, DOCS-02, DOCS-03, DOCS-04, BRAND-01, BRAND-02
**Success Criteria** (what must be TRUE):
  1. The language guide includes a section on interface declaration, `impl` blocks, and interface-typed variables with working code examples that compile with the current compiler
  2. The language guide includes a section on collection annotations (`unordered`, `layout: soa`, `layout: aos`) with examples showing when and why to use each
  3. A compiler internals page explains how the compiler constructs tagged unions, splits collections, and selects AoS/SoA layout -- aimed at contributors, not end users
  4. At least two concrete game-dev examples (e.g., particle system, entity component system) demonstrate the "write interfaces, get ECS performance" value proposition with before/after showing the Iron source and the generated C
  5. The site tagline reads "A Language Forged for Games" and the landing page highlights the static dispatch value proposition with concrete before/after code examples
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### v0.1.1-alpha Collection Methods, Full Captures & Layout Optimizations Phase Details

### Phase 46: Full Closure Capture
**Goal**: Lambdas can capture mutable variables by reference, be returned from functions, stored as object fields, nested inside other lambdas, and reference themselves recursively -- enabling the full closure expressiveness needed for collection method callbacks
**Depends on**: Phase 45
**Requirements**: CAPT-05, CAPT-06, CAPT-07, CAPT-08, CAPT-09, CAPT-10
**Success Criteria** (what must be TRUE):
  1. A lambda that captures a mutable `var` from its enclosing scope modifies the original variable; after the lambda executes, the outer scope observes the mutation
  2. A function that returns a closure can be called, and the returned closure executes correctly with its captured environment intact even after the creating function has returned
  3. An object with a closure-typed field retains the closure's captured environment across method calls; the closure executes correctly when invoked through the field
  4. A lambda nested inside another lambda correctly captures variables from both the immediate enclosing lambda and the outer function scope
  5. A recursive lambda defined via `var f = func(...) { ... f(...) ... }` compiles without infinite recursion at compile time and terminates correctly at runtime; two closures sharing the same mutable variable observe each other's mutations
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 47: Collection Methods
**Goal**: Arrays support `.map()`, `.filter()`, `.reduce()`, `.forEach()`, and `.sum()` with method syntax and lambda arguments, including chaining and dispatch across interface-typed split collections
**Depends on**: Phase 46
**Requirements**: COLL-05, COLL-06, COLL-07, COLL-08, COLL-09, COLL-10, COLL-11
**Success Criteria** (what must be TRUE):
  1. `arr.map(func(x: Int) -> Int { return x * 2 })` returns a new array with each element doubled; the original array is unmodified
  2. `arr.filter(func(x: Int) -> Bool { return x > 5 })` returns a new array containing only elements greater than 5; `arr.reduce(0, func(acc: Int, x: Int) -> Int { return acc + x })` returns the sum
  3. `arr.forEach(func(x: Int) { print(x) })` iterates all elements with side effects and returns no value; `arr.sum()` returns the numeric total for Int and Float arrays
  4. `arr.map(...).filter(...).sum()` compiles and produces the correct result by chaining method calls left to right
  5. Collection methods invoked on an interface-typed split collection (e.g., `shapes.map(...)`) dispatch the lambda per concrete type across all sub-arrays and produce a correct combined result
**Plans:** 3/3 plans executed (COMPLETE)

Plans:
- [x] 47-01-PLAN.md — Generic array extension method syntax (parser, resolver, type checker)
- [x] 47-02-PLAN.md — Collection method compilation + stdlib Iron implementations
- [x] 47-03-PLAN.md — Method chaining + split collection dispatch + integration tests

### Phase 48: Layout Optimizations
**Goal**: The compiler selects optimal memory layout per collection based on access patterns, eliminates unused fields from storage structs, factors common fields, and handles size-disparate variants efficiently
**Depends on**: Phase 41 (collection splitting from v0.1-alpha)
**Requirements**: LAYOUT-01, LAYOUT-02, LAYOUT-03, LAYOUT-04, LAYOUT-05
**Success Criteria** (what must be TRUE):
  1. A loop that accesses only 1-2 fields of a collection element causes the compiler to select SoA layout for that type's storage; a loop that accesses all fields selects AoS layout; the selection happens automatically with no annotation
  2. A field declared on an implementor but never read or written through any interface-typed collection operation is excluded from that collection's storage struct; direct concrete-type usage of the field still works
  3. Fields with the same name, type, and position across all implementors of an interface are stored in a single shared array rather than duplicated per type sub-array
  4. A non-collection interface variable where one implementor is significantly larger than others stores the large variant via pointer indirection while small variants remain inline
  5. `layout: soa` and `layout: aos` annotations on collections override the automatic selection; when the annotation contradicts the compiler's analysis, a warning is emitted
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 49: Loop Fusion & Monomorphic Specialization
**Goal**: Chained collection operations execute in a single fused loop per concrete type with no intermediate allocations, and single-type collections collapse to plain typed arrays
**Depends on**: Phase 47
**Requirements**: FUSE-01, FUSE-02, MONO-01, MONO-02
**Success Criteria** (what must be TRUE):
  1. `arr.map(...).filter(...).reduce(...)` compiles to a single fused loop per concrete type that applies map, filter, and reduce in one pass; no intermediate array is allocated between chained operations
  2. The fused loop produces results identical to executing each operation separately for all input types and edge cases (empty arrays, all-filtered-out, single element)
  3. A collection proven by dataflow analysis to hold only one concrete type collapses to a plain typed array with direct field access; no tag dispatch or split collection overhead remains
  4. The specialization registry prevents duplicate emission of the same specialized function body for a given (function, concrete_type) pair
**Plans**: 3 plans

Plans:
- [ ] 49-01-PLAN.md — @fusible annotation pipeline + chain detection + spec doc
- [ ] 49-02-PLAN.md — Fused loop emission (flat + split) + fusion tests
- [ ] 49-03-PLAN.md — Monomorphic collapse + specialization registry + tests

### Phase 50: Value Range Compression & Arena Allocation
**Goal**: Fields proven to fit in narrower types are stored compressed in collection structs, and per-type sub-arrays use arena allocation for efficient bulk memory management
**Depends on**: Phase 41 (collection splitting from v0.1-alpha)
**Requirements**: VRC-01, VRC-02, ARENA-01, ARENA-02
**Success Criteria** (what must be TRUE):
  1. A field proven by whole-program analysis to hold only values 0-255 is stored as `uint8_t` in the collection storage struct instead of a full `int64_t`; the generated C shows the narrowed type
  2. Access sites for compressed fields emit widening reads (cast to original type on load) and narrowing writes (cast to compressed type on store); the program produces identical results to the non-compressed version
  3. Per-type sub-arrays in split collections use arena allocation with geometric growth (capacity doubling); the generated C shows arena-based allocation calls instead of individual malloc per element
  4. Arena free releases all per-type sub-arrays for a split collection in a single operation; no per-element free calls are emitted
**Plans**: 3 plans
Plans:
- [ ] 50-01-PLAN.md — Value range analysis module and compressed field code generation
- [ ] 50-02-PLAN.md — Arena allocation with pointer registry for split collections
- [ ] 50-03-PLAN.md — Integration tests for value range compression and arena allocation


## Progress

**Execution Order:**
Phases execute in numeric order: 40 -> 41 -> 42 -> 43 -> 44 -> 45 -> 46 -> 47 -> 48 -> 49 -> 50 -> 51 -> 52 -> 53 -> 54

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1. Frontend | v0.0.1-alpha | 4/4 | Complete | 2026-03-25 |
| 2. Semantics and Codegen | v0.0.1-alpha | 8/8 | Complete | 2026-03-26 |
| 3. Runtime, Stdlib, and CLI | v0.0.1-alpha | 8/8 | Complete | 2026-03-26 |
| 4. Comptime, Game Dev, and Cross-Platform | v0.0.1-alpha | 6/6 | Complete | 2026-03-26 |
| 5. Codegen Fixes + Stdlib Wiring | v0.0.1-alpha | 5/5 | Complete | 2026-03-26 |
| 6. Milestone Gap Closure | v0.0.1-alpha | 2/2 | Complete | 2026-03-27 |
| 7. IR Foundation | v0.0.2-alpha | 2/2 | Complete | 2026-03-27 |
| 8. AST-to-IR Lowering | v0.0.2-alpha | 3/3 | Complete | 2026-03-27 |
| 9. C Emission and Cutover | v0.0.2-alpha | 4/4 | Complete | 2026-03-27 |
| 10. Test Hardening | v0.0.2-alpha | 5/5 | Complete | 2026-03-27 |
| 11. Release Pipeline & Versioning | v0.0.2-alpha | 2/2 | Complete | 2026-03-27 |
| 12. Binary Split and Installation | v0.0.3-alpha | 3/3 | Complete | 2026-03-28 |
| 13. Project Workflow | v0.0.3-alpha | 2/2 | Complete | 2026-03-28 |
| 14. Dependency Resolution and Lockfile | v0.0.3-alpha | 2/2 | Complete | 2026-03-28 |
| 15. Copy Propagation, DCE & Constant Folding | v0.0.5-alpha | 3/3 | Complete | 2026-03-29 |
| 16. Expression Inlining | v0.0.5-alpha | 3/3 | Complete | 2026-03-29 |
| 17. Strength Reduction & Store/Load Elimination | v0.0.5-alpha | 3/3 | Complete | 2026-03-29 |
| 18. Benchmark Validation | v0.0.5-alpha | 4/4 | Complete | 2026-03-30 |
| 19. LIR Rename & HIR Foundation | v0.0.5-alpha | 3/3 | Complete | 2026-03-30 |
| 20. HIR Lowering & Pipeline Cutover | v0.0.5-alpha | 7/7 | Complete | 2026-03-30 |
| 21. Parallel-For Fix | v0.0.6-alpha | 1/1 | Complete | 2026-03-31 |
| 22. Struct Codegen Fix | v0.0.6-alpha | 2/2 | Complete | 2026-03-31 |
| 23. Correctness Audit | v0.0.6-alpha | 2/2 | Complete | 2026-03-31 |
| 24. Range Bound Hoisting | v0.0.7-alpha | 1/1 | Complete | 2026-03-31 |
| 25. Stack Array Promotion | v0.0.7-alpha | 1/1 | Complete | 2026-04-01 |
| 26. LOAD Expression Inlining | v0.0.7-alpha | 1/1 | Complete | 2026-04-01 |
| 27. Function Inlining | v0.0.7-alpha | 2/2 | Complete | 2026-04-01 |
| 28. Phi Elimination Improvement | v0.0.7-alpha | 1/1 | Complete | 2026-04-01 |
| 29. Sized Integers | v0.0.7-alpha | 2/2 | Complete | 2026-04-01 |
| 30. Benchmark Validation and Exploration | v0.0.7-alpha | 2/2 | Complete | 2026-04-01 |
| 31. Spawn/Await Correctness | v0.0.7-alpha | 2/2 | Complete | 2026-04-01 |
| 32. LIR Verifier Hardening | v0.0.8-alpha | 2/2 | Complete | 2026-04-02 |
| 33. Type Validation Checks | v0.0.8-alpha | 3/3 | Complete | 2026-04-03 |
| 34. Bounds Checking | v0.0.8-alpha | 2/2 | Complete | 2026-04-03 |
| 35. Escape Analysis Extension | v0.0.8-alpha | 1/1 | Complete | 2026-04-03 |
| 36. Definite Assignment Analysis | v0.0.8-alpha | 2/2 | Complete | 2026-04-03 |
| 37. Generic Constraint Checking | v0.0.8-alpha | 2/2 | Complete | 2026-04-03 |
| 38. Concurrency Safety | v0.0.8-alpha | 2/2 | Complete | 2026-04-03 |
| 39. Diagnostic Test Sweep | v0.0.8-alpha | 2/2 | Complete | 2026-04-04 |
| 40. Syntax & Core Dispatch | v0.1-alpha | 0/0 | Not started | - |
| 41. Collection Splitting | v0.1-alpha | 0/0 | Not started | - |
| 42. Memory Layout | v0.1-alpha | 0/0 | Not started | - |
| 43. Loop Fusion & Monomorphic Specialization | v0.1-alpha | 0/0 | Not started | - |
| 44. Hardware Acceleration & Value Optimization | v0.1-alpha | 0/0 | Not started | - |
| 45. Documentation & Branding | v0.1-alpha | 0/0 | Not started | - |
| 46. Full Closure Capture | v0.1.1-alpha | 0/0 | Not started | - |
| 47. Collection Methods | 3/3 | Complete |  | - |
| 48. Layout Optimizations | 4/4 | Complete    | 2026-04-07 | - |
| 49. Loop Fusion & Monomorphic Specialization | 3/3 | Complete    | 2026-04-08 | - |
| 50. Value Range Compression & Arena Allocation | 3/3 | Complete    | 2026-04-08 | - |
| 51. Memory Investigation & Leak Audit | v0.1.1-alpha | 0/0 | Complete | 2026-04-08 |
| 52. Emitter Refactoring | 3/3 | Complete    | 2026-04-08 | - |
| 53. Analysis Improvements | 3/3 | Complete    | 2026-04-09 | - |
| 54. Test Hardening | 2/3 | Complete    | 2026-04-09 | - |
| 55. Push on Interface Arrays | 3/3 | Complete    | 2026-04-09 | - |
| 55.1. Empty Typed Array Literal | 1/1 | Complete    | 2026-04-09 | - |
| 56. Monomorphic Method Chain | 2/2 | Complete    | 2026-04-09 | - |
| 57. SoA + Fusion Composition | 2/3 | In Progress|  | - |

### Phase 51: Memory Investigation & Leak Audit

**Goal**: Investigate and fix the source of extreme memory consumption (50GB+) observed during ironc compilation — determine whether the leak is in the compiler itself (arena allocations, stb_ds hash maps, generated code buffers) or in the generated C program, and fix it
**Depends on**: Phase 50
**Requirements**: MEM-01, MEM-02
**Success Criteria** (what must be TRUE):
  1. ironc compiling any integration test program uses less than 500MB of peak memory (measured via `/usr/bin/time -l` or equivalent)
  2. No memory leak detected by running the generated C program under AddressSanitizer or valgrind — all allocated memory is freed or accounted for
  3. Root cause identified and documented — whether compiler-side (arena, stb_ds, strbuf growth) or generated-code-side (missing free, unbounded growth)
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### v0.1.2-alpha Compiler Hardening & Refactoring

### Phase 52: Emitter Refactoring
**Goal**: The monolithic emit_c.c is decomposed into focused sub-modules with clean APIs, making each emission concern independently readable, testable, and maintainable
**Depends on**: Phase 51
**Requirements**: EMIT-01, EMIT-02, EMIT-03, EMIT-04
**Success Criteria** (what must be TRUE):
  1. Split collection emission (struct generation, push functions, free, iteration) lives in a dedicated `emit_split.c` file with a public API header; `emit_c.c` calls into it rather than containing that logic inline
  2. Fusion emission (fused loop generation, chain detection helpers) lives in a dedicated `emit_fusion.c` file; all fusion-related code paths route through this module
  3. Struct/layout emission (object struct bodies, SoA/AoS arrays, storage structs, type emission) lives in a dedicated `emit_structs.c` file; struct generation logic is no longer interleaved with dispatch or iteration code
  4. All EmitCtx fields are documented with comments explaining their purpose, consistently named (no mixed conventions), and a single `emit_ctx_cleanup()` function releases all resources -- no scattered cleanup code remains
  5. All existing integration tests pass identically after the refactoring -- zero behavioral changes
**Plans**: 3 plans

Plans:
- [ ] 52-01-PLAN.md — Extract emit_helpers.c/h (EmitCtx + shared utilities + emit_ctx_cleanup)
- [ ] 52-02-PLAN.md — Extract emit_structs.c/h (type declarations + tagged unions)
- [ ] 52-03-PLAN.md — Extract emit_split.c/h + emit_fusion.c/h (split collections + fused loops)

### Phase 53: Analysis Improvements
**Goal**: Monomorphic detection and value range analysis cross function boundaries, enabling optimizations that currently bail out at call sites
**Depends on**: Phase 52
**Requirements**: ANAL-01, ANAL-02, ANAL-03
**Success Criteria** (what must be TRUE):
  1. A helper function that constructs and returns a single-concrete-type collection triggers monomorphic collapse at the call site -- the caller's collection is optimized as if it were locally constructed
  2. A function that always returns values in a narrow range (e.g., 0-100) causes value range analysis at the call site to use that range instead of conservative TOP -- the field storing the return value compresses to the appropriate narrowed type
  3. A conditional branch `if x < 100` narrows the range of `x` to [min, 99] inside the true branch and [100, max] inside the false branch; value range compression uses these narrowed ranges for fields assigned within the branch
  4. Existing programs that do not cross function boundaries produce identical output -- the interprocedural extensions are additive, not behavior-changing
**Plans**: 3 plans

Plans:
- [ ] 53-01-PLAN.md — Return range propagation and conditional branch narrowing in value_range.c
- [ ] 53-02-PLAN.md — Interprocedural monomorphic detection with specialization heuristic
- [ ] 53-03-PLAN.md — Gap closure: wire func_return_types into Phase A coll_types + update interprocedural tests

### Phase 54: Test Hardening
**Goal**: Edge cases, stress conditions, and optimization compositions are systematically tested so regressions cannot hide in untested corners
**Depends on**: Phase 53
**Requirements**: TEST-01, TEST-02, TEST-03
**Success Criteria** (what must be TRUE):
  1. Edge case tests exist and pass for: empty collections through map/filter/reduce/sum, all-filtered-out producing empty results, single-element collections, zero-field structs in split collections, and single-implementor interfaces collapsing correctly
  2. Stress tests exist and pass for: collections with 10K+ elements producing correct results, interfaces with 10+ implementors dispatching correctly, and deeply nested fusion chains (5+ chained operations) fusing into single loops
  3. Composition tests exist and pass for: SoA layout + fusion (fused loop reads from per-field arrays), dead field elimination + value range compression (compressed fields exclude dead ones), monomorphic collapse + fusion (single-type fused loops), and arena allocation + SoA (arena-backed per-field arrays)
  4. All tests are automated and run as part of the standard `make test` or equivalent CI command -- no manual verification steps
**Plans**: 3 plans

Plans:
- [x] 54-01-PLAN.md — Edge case tests (empty collections, all-filtered-out, single element, single implementor)
- [x] 54-02-PLAN.md — Stress tests (large collections, many implementors, deep fusion chains)
- [x] 54-03-PLAN.md — Composition tests (SoA+fusion, dead field+compression, mono+fusion, arena+SoA+dead, mega)

### Phase 55: Push on Interface Arrays

**Goal**: Root-cause and fix `.push()` on interface-typed split collections — programmatic building of `[Shape]` via push loop must compile and run correctly
**Depends on**: Phase 54
**Requirements**: PUSH-01, PUSH-02
**Success Criteria** (what must be TRUE):
  1. `var shapes = [Circle(1), Square(2)]; shapes.push(Circle(5))` compiles and runs correctly (non-empty initial literal exercises the push dispatch fix; empty typed literal `var shapes: [Shape] = []` is tracked separately under Phase 55.1 / EMPTY-LIT)
  2. Push loop building a 100+ element split collection produces correct runtime output
  3. Root cause of the original bug documented in commit message (what was broken, why, how fixed)
  4. Regression test `push_interface_collection.iron` exists and exercises the previously-broken path
  5. Adjacent tests: push with multiple interface types, push after other operations, push into an already-populated collection
**Plans**: 3 plans

Plans:
- [x] 55-01-PLAN.md — Fix .push() on interface split collections (concrete + interface-typed arg dispatch) + main regression + adjacent tests + 100-element stress test
- [x] 55-02-PLAN.md — Add .len() and .pop() branches to the interception block + tests
- [x] 55-03-PLAN.md — Add .get() and .set() branches (same-type set semantics) + tests

### Phase 55.1: Empty Typed Array Literal

**Goal**: Fix empty typed array literal `[]` type inference so that `var shapes: [Shape] = []` compiles. Surfaced during Phase 55 verification as orthogonal to the push dispatch fix — isolated into its own decimal phase to keep scope clean.
**Depends on**: Phase 55
**Requirements**: EMPTY-LIT-01, EMPTY-LIT-02
**Success Criteria** (what must be TRUE):
  1. `var shapes: [Shape] = []` compiles without errors when the variable has an explicit interface array annotation
  2. `var shapes: [Shape] = []; shapes.push(Circle(5)); shapes.push(Square(3)); println("{shapes.len()}")` compiles and prints `2`
  3. Same works for concrete element types: `var ints: [Int] = []; ints.push(7)` compiles
  4. Root cause documented in commit message — current failure is `E0202: type mismatch expected '[<interface>]', got '[<error>]'`; the empty literal path in `src/analyzer/typecheck.c` does not honor the declared annotation
  5. Regression test `empty_interface_var_push.iron` covering interface element type; companion tests `empty_concrete_var_push.iron`, `empty_literal_call_arg.iron`, `empty_literal_return.iron` for concrete + call-arg + return contexts
**Plans**: 1 plan

Plans:
- [x] 55.1-01-PLAN.md — Add check_expr_with_expected helper + IRON_ERR_EMPTY_LITERAL_NO_TYPE (229) diagnostic; swap 4 callsites (var decl, call arg, return, assign); add 4 regression tests

### Phase 56: Monomorphic Method Chain

**Goal**: Root-cause and fix the monomorphic-collapsed collection + method chain bug (`.map()`, `.filter()` chain failures after mono collapse)
**Depends on**: Phase 55
**Requirements**: MONO-FIX-01, MONO-FIX-02
**Success Criteria** (what must be TRUE):
  1. Monomorphic-collapsed single-element collections with `.map()` chain compile and run correctly
  2. Monomorphic single-implementor interfaces with full method chains produce correct results
  3. Root cause documented — was it the codegen path, type propagation, or dispatch lowering?
  4. Regression test `mono_method_chain.iron` exists with explicit mono collapse + chain
  5. Adjacent tests: mono + fusion, mono + multiple methods, mono with different concrete types
**Plans**: 2 plans

Plans:
- [x] 56-01-PLAN.md — Primary fix (IRON_LIST_DECL pre-scan in emit_structs.c) + regression test mono_method_chain.iron + full method parity sweep + chain combinations + fusion probe
- [x] 56-02-PLAN.md — Narrowing audit (typecheck.c .push arg validation) + positive/negative tests + Phase 55 workaround cleanup

### Phase 57: SoA + Fusion Composition

**Goal**: Root-cause and fix the SoA layout + fusion Stor type mismatch
**Depends on**: Phase 56
**Requirements**: SOA-FIX-01, SOA-FIX-02
**Success Criteria** (what must be TRUE):
  1. Split collections with SoA layout annotation fuse correctly with `.map().filter()` chains
  2. The original `compose_soa_fusion.iron` pattern (previously worked around) runs directly without adaptation
  3. Root cause documented — why does the Stor type reference mismatch occur when SoA + fusion combine?
  4. Regression test exercises SoA + fusion + reduce/sum
  5. Adjacent tests: SoA + fusion + dead field, SoA + fusion + compression, SoA + fusion on split with many types
**Plans**: 3 plans

Plans:
- [x] 57-01-PLAN.md — Atomic core fix (emit_structs.c _from_<Type>_Stor sibling ctor + emit_fusion.c reduced-storage branch) + primary regression soa_fusion_map_sum.iron — complete 2026-04-09
- [ ] 57-02-PLAN.md — Adjacent regression tests for SC5 corners: soa_fusion_dead_field, soa_fusion_compressed, soa_fusion_many_types (depends on 57-01)
- [ ] 57-03-PLAN.md — Restore fusion chains in compose_soa_fusion.iron and compose_mega.iron (previously for-loop workarounds; depends on 57-01)

### Phase 58: Benchmark Stabilization

**Goal**: Root-cause `binary_tree_diameter` performance gap and either fix or document it
**Depends on**: Phase 57
**Requirements**: BENCH-01, BENCH-02
**Success Criteria** (what must be TRUE):
  1. Root cause of the Iron/C performance gap documented with evidence (profiling data, generated C inspection)
  2. Either: gap closed to <1.5x ratio (threshold lowered back from 2.5x) — OR — gap documented as inherent with specific technical reason and threshold retained
  3. Benchmark runs 5 times consecutively on same machine with stable results (variance <5% across runs)
  4. If fix applied: regression benchmark ensures the fix holds across future changes
**Plans**: TBD

