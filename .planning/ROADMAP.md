# Roadmap: Iron

## Overview

Iron is built in four broad phases that follow the natural dependency graph of a transpile-to-C compiler. The frontend (lexer, parser) must exist before analysis; analysis must be complete before codegen can emit correct C; the runtime and toolchain turn the compiler into something you can actually use; and the differentiator features (comptime, raylib, cross-platform) cap the v1 release. Each phase produces something independently verifiable. No phase ships until its success criteria are observable.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [x] **Phase 1: Frontend** - Lexer and parser produce a complete, span-annotated AST from any Iron source file (completed 2026-03-25)
- [x] **Phase 2: Semantics and Codegen** - Fully analyzed AST emits valid C11 that compiles and runs (completed 2026-03-26)
- [x] **Phase 3: Runtime, Stdlib, and CLI** - Iron programs are buildable, runnable, and testable from the command line (completed 2026-03-26)
- [x] **Phase 4: Comptime, Game Dev, and Cross-Platform** - Comptime evaluation, raylib bindings, and Windows parity complete v1 (completed 2026-03-26)
- [x] **Phase 5: Codegen Fixes + Stdlib Wiring** - Fix string interpolation and parallel-for codegen; wire stdlib modules to Iron source via import (completed 2026-03-26)

## Phase Details

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
- [x] 01-01-PLAN.md — Project scaffolding, arena allocator, diagnostics, build system
- [x] 01-02-PLAN.md — Complete lexer (all tokens, spans, errors, comments)
- [x] 01-03-PLAN.md — AST node types and recursive descent parser
- [x] 01-04-PLAN.md — String interpolation, error recovery, pretty-printer, diagnostic tests

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
- [x] 02-01-PLAN.md — Type system and scope tree infrastructure
- [x] 02-02-PLAN.md — Two-pass name resolution with forward references
- [x] 02-03-PLAN.md — Type checking with inference, immutability, and nullable narrowing
- [x] 02-04-PLAN.md — Escape analysis and concurrency checking
- [x] 02-05-PLAN.md — Core C code generator (variables, functions, objects, defer, nullable)
- [x] 02-06-PLAN.md — Advanced codegen (vtables, monomorphization, lambdas, concurrency)
- [x] 02-07-PLAN.md — Unified pipeline, unit tests, and integration test fixtures
- [ ] 02-08-PLAN.md — Gap closure: fix codegen bugs, add auto_free emission, clang verification

### Phase 3: Runtime, Stdlib, and CLI
**Goal**: Iron programs can be built, run, checked, formatted, and tested via the `iron` CLI, backed by a complete runtime and standard library
**Depends on**: Phase 2
**Requirements**: RT-01, RT-02, RT-03, RT-04, RT-05, RT-06, RT-07, RT-08 (macOS+Linux only; Windows deferred to Phase 4), STD-01, STD-02, STD-03, STD-04, CLI-01, CLI-02, CLI-03, CLI-04, CLI-05, CLI-06, CLI-07, CLI-08, TEST-04
**Success Criteria** (what must be TRUE):
  1. `iron build hello.iron` produces a standalone native binary that executes; `iron run hello.iron` does the same in one step
  2. String, List, Map, Set, and Rc work correctly in Iron programs; the runtime passes all tests on macOS and Linux
  3. Standard library modules (`math`, `io`, `time`, `log`) are importable in Iron programs and their functions produce correct results
  4. Error messages display Rust-style diagnostics: source snippet, arrow pointing to the problem, and a suggestion; terminal output is colored; `--verbose` shows generated C
  5. The CI build runs with ASan+UBSan enabled and all tests pass clean (zero sanitizer errors)
**Plans:** 8/8 plans complete

Plans:
- [ ] 03-01-PLAN.md — Iron_String (SSO + interning), Iron_Rc, builtins, codegen print/println fix
- [ ] 03-02-PLAN.md — Thread pool, channels, mutex, handle (pthreads)
- [ ] 03-03-PLAN.md — Generic collections (List, Map, Set) with macro expansion
- [ ] 03-04-PLAN.md — Codegen runtime integration (includes, parallel-for fix, init/shutdown)
- [ ] 03-05-PLAN.md — Standard library modules (math, io, time, log)
- [ ] 03-06-PLAN.md — CLI core (build, run, check commands)
- [ ] 03-07-PLAN.md — CLI fmt, test runner, iron.toml parser
- [ ] 03-08-PLAN.md — GitHub Actions CI, integration tests, ASan/UBSan hardening

### Phase 4: Comptime, Game Dev, and Cross-Platform
**Goal**: Comptime evaluation works, raylib programs build and run, and the full toolchain produces binaries on macOS, Linux, and Windows
**Depends on**: Phase 3
**Requirements**: CT-01, CT-02, CT-03, CT-04, GAME-01, GAME-02, GAME-03, GAME-04, RT-08 (Windows parity)
**Success Criteria** (what must be TRUE):
  1. A `comptime` expression (e.g., `comptime fibonacci(10)`) evaluates at compile time and emits the result as a literal in the generated C; a step-limit violation produces a compile error rather than hanging
  2. `comptime read_file("assets/shader.glsl")` embeds the file contents as a string literal at compile time, resolved relative to the source file
  3. An Iron program that `import raylib` and uses the `draw {}` block compiles to a standalone binary that opens a window and handles input on macOS and Linux
  4. `iron build`, `iron run`, `iron check`, `iron fmt`, and `iron test` all produce correct results on macOS, Linux, and Windows
**Plans:** 6/6 plans complete

Plans:
- [ ] 04-01-PLAN.md — extern func declarations and draw {} block (lexer, parser, resolver, codegen)
- [ ] 04-02-PLAN.md — Comptime tree-walking AST interpreter with step limit and restrictions
- [ ] 04-03-PLAN.md — raylib.iron wrapper, build pipeline raylib compilation, integration tests
- [ ] 04-04-PLAN.md — Comptime read_file() builtin and content-hash cache
- [ ] 04-05-PLAN.md — Windows parity (C11 threads, clang-cl, platform CI)
- [ ] 04-06-PLAN.md — Gap closure: fix raylib.iron syntax, vendor raylib 5.5, pthreads-only runtime

### Phase 5: Codegen Fixes + Stdlib Wiring
**Goal**: String interpolation and parallel-for produce correct output; stdlib modules (math, io, time, log) are callable from Iron source via import
**Depends on**: Phase 4
**Requirements**: GEN-01 (string interpolation codegen), GEN-11, STD-01, STD-02, STD-03, STD-04
**Gap Closure**: Closes gaps from v1.0 milestone audit
**Success Criteria** (what must be TRUE):
  1. `"value is {x}"` where x=42 produces the string "value is 42" at runtime (not empty string)
  2. `parallel for i in range(100) { array[i] = i }` correctly distributes the range across pool workers — each iteration runs exactly once with the correct index
  3. `import math` followed by `val s = sin(1.0)` compiles and produces the correct result
  4. `import io` followed by `val content, err = read_file("test.txt")` compiles and returns file contents
  5. `import time` and `import log` modules are callable from Iron source
**Plans:** 5/5 plans complete

Plans:
- [x] 05-01-PLAN.md — String interpolation codegen (snprintf two-pass formatting)
- [x] 05-02-PLAN.md — Parallel-for codegen fix (capture struct, correct chunk signature)
- [x] 05-03-PLAN.md — Stdlib wiring (auto-static dispatch, .iron wrappers, build pipeline)
- [x] 05-04-PLAN.md — Integration tests (interpolation, parallel-for, math, time, log, hello.iron)
- [ ] 05-05-PLAN.md — Gap closure: fix IO.read_file return type mismatch, add read_file integration test

## Progress

**Execution Order:**
Phases execute in numeric order: 1 -> 2 -> 3 -> 4 -> 5

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Frontend | 4/4 | Complete    | 2026-03-26 |
| 2. Semantics and Codegen | 7/8 | Complete    | 2026-03-26 |
| 3. Runtime, Stdlib, and CLI | 8/8 | Complete    | 2026-03-26 |
| 4. Comptime, Game Dev, and Cross-Platform | 6/6 | Complete    | 2026-03-26 |
| 5. Codegen Fixes + Stdlib Wiring | 5/5 | Complete   | 2026-03-26 |
