# Roadmap: Iron

## Milestones

- v1.0 Iron Language Compiler - Phases 1-6 (shipped 2026-03-27)
- v1.1 High IR - Phases 7-11 (in progress)

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

<details>
<summary>v1.0 Iron Language Compiler (Phases 1-6) - SHIPPED 2026-03-27</summary>

- [x] **Phase 1: Frontend** - Lexer and parser produce a complete, span-annotated AST from any Iron source file (completed 2026-03-25)
- [x] **Phase 2: Semantics and Codegen** - Fully analyzed AST emits valid C11 that compiles and runs (completed 2026-03-26)
- [x] **Phase 3: Runtime, Stdlib, and CLI** - Iron programs are buildable, runnable, and testable from the command line (completed 2026-03-26)
- [x] **Phase 4: Comptime, Game Dev, and Cross-Platform** - Comptime evaluation, raylib bindings, and Windows parity complete v1 (completed 2026-03-26)
- [x] **Phase 5: Codegen Fixes + Stdlib Wiring** - Fix string interpolation and parallel-for codegen; wire stdlib modules to Iron source via import (completed 2026-03-26)
- [x] **Phase 6: Milestone Gap Closure** - Close remaining v1.0 audit gaps: range builtin, Timer wrappers, iron check stdlib support (completed 2026-03-27)

</details>

### v1.1 High IR

- [ ] **Phase 7: IR Foundation** - IR data structures, instruction set definition, printer, and verifier provide the complete scaffold for lowering and emission
- [ ] **Phase 8: AST-to-IR Lowering** - Every Iron language feature lowers from AST to typed SSA-form IR instructions
- [ ] **Phase 9: C Emission and Cutover** - IR-to-C backend produces equivalent output to old codegen; old codegen removed after parity gate
- [ ] **Phase 10: Test Hardening** - Reorganize test structure, add real algorithm tests, comprehensive IR coverage tests, and real-world composite programs
- [ ] **Phase 11: Release Pipeline & Versioning** - CI builds release binaries on draft release; `iron --version` shows commit hash and build date

## Phase Details

<details>
<summary>v1.0 Phase Details (Phases 1-6)</summary>

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
- [ ] 07-01-PLAN.md — IR data structures, IronIR_InstrKind enum, constructors, stub files, CMake wiring
- [ ] 07-02-PLAN.md — IR printer (LLVM-style), verifier (6 invariants), unit tests

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
**Plans:** 2/3 plans executed

Plans:
- [ ] 08-01-PLAN.md — Lowering infrastructure (POISON, LowerCtx, two-pass orchestration) and expression lowering (constants, arithmetic, comparisons, unary, calls, field/index, object/array, cast, nullable, string interpolation, slice, short-circuit and/or)
- [ ] 08-02-PLAN.md — Control flow lowering (if/else, while, for, match, return) and memory lowering (heap alloc, rc, free, defer eager inlining) plus lowering unit tests
- [ ] 08-03-PLAN.md — Module declarations (types, externs, monomorphization), concurrency lifting (lambda, spawn, parallel-for), draw keyword removal, and expanded tests

### Phase 9: C Emission and Cutover
**Goal**: The IR-to-C emission backend replaces the old AST-to-C codegen, producing identical program behavior verified by all existing integration tests
**Depends on**: Phase 8
**Requirements**: EMIT-01, EMIT-02, EMIT-03
**Success Criteria** (what must be TRUE):
  1. `iron build` compiles any valid Iron program through the AST-to-IR-to-C pipeline; the emitted C compiles with `clang -std=c11 -Wall -Werror` with zero warnings
  2. All existing integration tests pass through the new IR pipeline with identical runtime behavior (same stdout, same exit codes)
  3. The old `src/codegen/` AST-to-C path is fully removed from the codebase; no dual-path maintenance burden remains; the compiler has exactly one codegen path (IR-to-C)
**Plans**: TBD

Plans:
- [ ] 09-01: IR-to-C emission backend (emit_c.c) with phi pre-elimination, goto-based terminators, and sequential value naming
- [ ] 09-02: Integration test parity gate and old codegen removal

### Phase 10: Test Hardening
**Goal**: Reorganize the test directory, add real algorithm tests and comprehensive IR coverage tests so that the new IR pipeline is proven correct across a wide range of programs — from isolated instruction-level tests to real-world composite programs
**Depends on**: Phase 9
**Requirements**: THARD-01, THARD-02, THARD-03, THARD-04, THARD-05, THARD-06, THARD-07, THARD-08, THARD-09, THARD-10
**Success Criteria** (what must be TRUE):
  1. Test directory has clear structure (unit/, integration/, algorithms/, ir/) with consistent naming; all existing tests migrated without breakage; CMake/CI updated
  2. Real algorithm tests (quicksort, subset sum, binary search, fibonacci, merge sort, BFS/DFS) compile through the IR pipeline and produce correct output
  3. Every IR instruction kind has a dedicated lowering unit test with a minimal .iron snippet; every verifier invariant has a negative test that triggers the specific error
  4. Edge case tests cover deeply nested control flow, multiple early returns with defer, heap auto_free in loops, rc patterns, lambda capture of mutables, nested spawn/await, and generic stress patterns
  5. Real-world composite programs (mini game loop, CLI file processor, concurrent pipeline) compile and run correctly; IR printer snapshot tests catch regressions in IR output
**Plans**: TBD

Plans:
- [ ] 10-01: Test directory reorganization and IR lowering unit tests (every instruction kind + verifier negative tests)
- [ ] 10-02: Real algorithm test suite and control flow / memory / concurrency edge case tests
- [ ] 10-03: Real-world composite programs and IR printer snapshot tests

### Phase 11: Release Pipeline & Versioning
**Goal**: CI automatically builds downloadable release binaries for all platforms when a draft GitHub release is created, and `iron --version` outputs version, commit hash, and build date in rustc style
**Depends on**: Phase 10
**Requirements**: REL-01, REL-02, REL-03, REL-04
**Success Criteria** (what must be TRUE):
  1. Creating a draft release on GitHub triggers a CI workflow that builds iron binaries for macOS (arm64 + x86_64), Linux (x86_64), and Windows (x86_64)
  2. Built binaries are uploaded as release assets and are directly downloadable and executable on each platform
  3. `iron --version` outputs `iron X.Y.Z (abcdef0 2026-MM-DD)` matching the rustc format with the actual commit hash and build date
  4. Commit hash and build date are baked in at compile time via CMake defines, not runtime detection
**Plans**: TBD

Plans:
- [ ] 11-01: Version string with commit hash and build date (CMake configure + CLI flag)
- [ ] 11-02: CI release workflow (GitHub Actions: build matrix, artifact upload on draft release)

## Progress

**Execution Order:**
Phases execute in numeric order: 7 -> 8 -> 9 -> 10 -> 11

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1. Frontend | v1.0 | 4/4 | Complete | 2026-03-25 |
| 2. Semantics and Codegen | v1.0 | 8/8 | Complete | 2026-03-26 |
| 3. Runtime, Stdlib, and CLI | v1.0 | 8/8 | Complete | 2026-03-26 |
| 4. Comptime, Game Dev, and Cross-Platform | v1.0 | 6/6 | Complete | 2026-03-26 |
| 5. Codegen Fixes + Stdlib Wiring | v1.0 | 5/5 | Complete | 2026-03-26 |
| 6. Milestone Gap Closure | v1.0 | 2/2 | Complete | 2026-03-27 |
| 7. IR Foundation | 2/2 | Complete   | 2026-03-27 | - |
| 8. AST-to-IR Lowering | 2/3 | In Progress|  | - |
| 9. C Emission and Cutover | v1.1 | 0/2 | Not started | - |
| 10. Test Hardening | v1.1 | 0/3 | Not started | - |
| 11. Release Pipeline & Versioning | v1.1 | 0/2 | Not started | - |
