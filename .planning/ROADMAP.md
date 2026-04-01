# Roadmap: Iron

## Milestones

- v0.0.1-alpha Iron Language Compiler - Phases 1-6 (shipped 2026-03-27)
- v0.0.2-alpha High IR - Phases 7-11 (shipped 2026-03-27)
- v0.0.3-alpha Package Manager - Phases 12-14 (shipped 2026-03-28)
- v0.0.5-alpha IR Optimization & High IR Architecture - Phases 15-20 (shipped 2026-03-30)
- v0.0.6-alpha HIR Pipeline Correctness - Phases 21-23 (shipped 2026-03-31)
- v0.0.7-alpha Performance Optimization - Phases 24-30 (active)

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
- [x] **Phase 23: Correctness Audit** - Systematic HIR→LIR→C audit identifies and fixes any remaining correctness gaps; new test cases cover all problem areas (completed 2026-03-31)

</details>

### v0.0.7-alpha Performance Optimization

- [ ] **Phase 24: Range Bound Hoisting** - `Iron_range()` is evaluated once in the loop pre-header; all for-range benchmarks show measurable speedup
- [ ] **Phase 25: Stack Array Promotion** - `fill(CONST, val)` with constant size promotes to `alloca()`-based stack allocation for non-escaping small arrays
- [ ] **Phase 26: LOAD Expression Inlining** - LOAD instructions are inline-eligible when their use is in the same block; cross-block LOADs remain excluded
- [x] **Phase 27: Function Inlining** - Small pure non-recursive functions are inlined at LIR level before the copy-prop/DCE fixpoint
- [ ] **Phase 28: Phi Elimination Improvement** - Copy coalescing in phi elimination reduces generated temporaries in complex control flow
- [ ] **Phase 29: Sized Integers** - `Int32` type annotation emits `int32_t` in generated C; array operations use 32-bit memory bandwidth
- [ ] **Phase 30: Benchmark Validation and Exploration** - Full benchmark suite run against pre-optimization baseline; exploration pass identifies any remaining opportunities
- [ ] **Phase 31: Spawn/Await Correctness** - Verify spawn/await semantics work end-to-end; fix concurrency benchmarks for fair timing

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
  1. `ironc build main.iron`, `ironc run main.iron`, `ironc check main.iron`, `ironc fmt main.iron`, and `ironc test tests/` all behave identically to the old `iron` equivalents — no behavior change, only a binary rename
  2. `iron build main.iron` continues to work and produce the same output as before (backward compat fallback); a file argument triggers single-file mode with an optional deprecation hint
  3. The `iron` binary discovers `ironc` at runtime by resolving its own executable path and checking for a sibling binary — no compile-time hardcoded paths, no `IRON_SOURCE_DIR` dependency
  4. `cmake --install` (or the install script) installs both `iron` and `ironc` to the same directory and adds that directory to PATH
**Plans:** 3/3 plans complete

Plans:
- [x] 12-01-PLAN.md — Rename CMake target to ironc, add runtime path resolution
- [x] 12-02-PLAN.md — Create iron package manager CLI with forwarding to ironc
- [x] 12-03-PLAN.md — Install pipeline, CI workflows, and end-to-end verification

### Phase 13: Project Workflow
**Goal**: Users can create, build, run, check, and test iron.toml projects using the `iron` CLI with Cargo-style colored output
**Depends on**: Phase 12
**Requirements**: PROJ-01, PROJ-02, PROJ-03, PROJ-04, PROJ-05, PROJ-06, DEP-01, DEP-02, CLI2-05
**Success Criteria** (what must be TRUE):
  1. `iron init` in an empty directory creates `iron.toml` (with `[package]` and empty `[dependencies]`), `src/main.iron`, and `.gitignore`; `iron init --lib` creates `src/lib.iron` instead
  2. `iron build` in a directory with `iron.toml` reads the manifest, collects source files, and produces a working binary via `ironc` — no dependency section required
  3. `iron run` builds and immediately executes the package binary; `iron check` reports type errors without producing any output file; `iron test` discovers and runs all tests in the package
  4. All `iron` status lines use Cargo-style formatting: `  Compiling name v0.1.0`, `   Finished dev in 0.42s`, `    Running ./target/name`; colors are suppressed when stdout is not a TTY or `NO_COLOR` is set; Windows enables VT processing at startup
  5. `iron.toml` accepts `[dependencies]` entries in the form `name = { git = "owner/repo", version = "X.Y.Z" }` and parses them without error (resolution deferred to Phase 14)
**Plans:** 2/2 plans complete

Plans:
- [x] 13-01-PLAN.md — TOML parser extension, color.h, ironc --output flag
- [x] 13-02-PLAN.md — iron init, build, run, check, test commands

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
- [x] 14-01-PLAN.md — Fetcher (GitHub API + tarball + cache) and lockfile (iron.lock read/write) infrastructure
- [x] 14-02-PLAN.md — DFS resolver, source concatenation, cmd_build integration, CMake tests

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
- [x] 15-01-PLAN.md — Extract passes from emit_c.c into ir_optimize module, wire CLI flags
- [x] 15-02-PLAN.md — Implement copy propagation, constant folding, DCE with fixpoint loop
- [x] 15-03-PLAN.md — Unit tests and .iron integration tests for all three passes

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
- [x] 16-01-PLAN.md — Use-count analysis, function purity, inline eligibility infrastructure, emit_expr_to_buf, unit tests
- [x] 16-02-PLAN.md — Integration tests for expression inlining across Iron features
- [x] 16-03-PLAN.md — Benchmark parity assertion for median_two_sorted_arrays

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
- [x] 17-01-PLAN.md — Store/load elimination with escape analysis, 3 unit tests
- [x] 17-02-PLAN.md — Dominator tree, loop analysis, strength reduction pass, 3 unit tests
- [x] 17-03-PLAN.md — Integration tests for both passes, full regression verification

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
- [x] 18-01-PLAN.md — Fix ARRAY_LIT inlining bug, add int32 emission-time narrowing
- [x] 18-02-PLAN.md — Benchmark runner --json output and --compare mode
- [x] 18-03-PLAN.md — 10 concurrency correctness benchmarks
- [x] 18-04-PLAN.md — Threshold tuning and full suite validation

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
- [x] 19-01-PLAN.md — LIR rename: move directories and rename all IR symbols to LIR
- [x] 19-02-PLAN.md — HIR data structures, constructors, and unit tests
- [x] 19-03-PLAN.md — HIR printer and verifier with snapshot and error tests

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
- [x] 20-01-PLAN.md — AST-to-HIR lowering implementation + smoke tests
- [x] 20-02-PLAN.md — HIR-to-LIR three-pass SSA lowering + smoke tests
- [x] 20-03-PLAN.md — Pipeline wiring in build.c + parity verification
- [x] 20-04-PLAN.md — Cutover: delete old lower files + cleanup
- [x] 20-05-PLAN.md — Unit test suite: AST-to-HIR (50 tests)
- [x] 20-06-PLAN.md — Unit test suite: HIR-to-LIR (50 tests)
- [x] 20-07-PLAN.md — Integration test suite (100 .iron tests)

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
- [x] 21-01-PLAN.md — Fix is_lifted_func() prefix check and verify all pfor tests pass

### Phase 22: Struct Codegen Fix
**Goal**: Struct values are passed cleanly through the HIR pipeline — no spurious constructor wrapping, no excess CONSTRUCT field emission — and all struct-dependent algorithm tests produce correct output
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
- [x] 22-01-PLAN.md — Fix both struct bugs: remove spurious constructor detection + add field count clamp
- [x] 22-02-PLAN.md — Add regression test and verify all algorithm/integration/composite tests pass

### Phase 23: Correctness Audit
**Goal**: A systematic review of the HIR→LIR→C pipeline surface area identifies any remaining correctness bugs, all found issues are fixed with regression tests, and new test cases cover the problem areas addressed this milestone
**Depends on**: Phase 22
**Requirements**: AUDIT-01, AUDIT-02, AUDIT-03
**Success Criteria** (what must be TRUE):
  1. Every HIR instruction kind and every emit_c.c emission path has been reviewed for correctness against the Iron language spec; the audit produces a written checklist of what was checked and what (if anything) was found
  2. Any correctness bug found during the audit is fixed with a targeted code change and a regression test that would have caught the original bug
  3. New .iron test cases cover parallel-for emission, struct value passing, and any other problem areas uncovered in phases 21-22 or the audit; the test suite grows by at least 5 new correctness-focused tests
**Plans:** 2/2 plans complete

Plans:
- [x] 23-01-PLAN.md — Systematic HIR→LIR→C audit and fix any additional issues
- [x] 23-02-PLAN.md — New correctness test cases for pfor, struct passing, and audit findings

</details>

### v0.0.7-alpha Phase Details

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
- [ ] 24-01-PLAN.md — Hoist GET_FIELD .count to pre-header and validate benchmarks

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
- [ ] 25-01-PLAN.md — Constant-count gate + declaration hoisting + validation

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
- [ ] 26-01-PLAN.md — Remove LOAD blanket exclusion, add regression test, validate full suite

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
- [x] 27-01-PLAN.md — Implement run_function_inlining with regression test

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
- [ ] 28-01-PLAN.md — Implement dead alloca elimination pass and validate temporary reduction

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
- [ ] 29-01-PLAN.md — Int32 type coercion, runtime list, integration tests
- [ ] 29-02-PLAN.md — connected_components Int32 rewrite and micro-benchmark

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
- [ ] 30-01-PLAN.md — Run full benchmark suite, archive baseline, update thresholds
- [ ] 30-02-PLAN.md — Benchmark analysis report, outlier deep-dive, performance doc update

## Progress

**Execution Order:**
Phases execute in numeric order: 24 -> 25 -> 26 -> 27 -> 28 -> 29 -> 30 -> 31

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
| 24. Range Bound Hoisting | 1/1 | Complete    | 2026-03-31 | - |
| 25. Stack Array Promotion | 1/1 | Complete    | 2026-04-01 | - |
| 26. LOAD Expression Inlining | 1/1 | Complete    | 2026-04-01 | - |
| 27. Function Inlining | 2/2 | Complete    | 2026-04-01 | 2026-03-31 |
| 28. Phi Elimination Improvement | 1/1 | Complete    | 2026-04-01 | - |
| 29. Sized Integers | 2/2 | Complete    | 2026-04-01 | - |
| 30. Benchmark Validation and Exploration | 2/2 | Complete    | 2026-04-01 | - |
| 31. Spawn/Await Correctness | 1/2 | In Progress|  | - |

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
**Plans:** 1/2 plans executed

Plans:
- [ ] 31-01-PLAN.md -- Runtime + compiler pipeline: spawn returns handle, await returns value, compiler warning
- [ ] 31-02-PLAN.md -- Benchmark updates + threshold re-validation + human verification
