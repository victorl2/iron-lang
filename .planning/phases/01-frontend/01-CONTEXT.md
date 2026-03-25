# Phase 1: Frontend - Context

**Gathered:** 2025-03-25
**Status:** Ready for planning

<domain>
## Phase Boundary

Lexer and parser produce a complete, span-annotated AST from any Iron source file. This phase delivers the compiler frontend — tokenization, parsing, error recovery, and AST representation. Semantic analysis, code generation, and runtime are separate phases.

</domain>

<decisions>
## Implementation Decisions

### Error Diagnostic Format
- 3-line context window: line above, error line, line below (Rust-style default)
- Fix suggestions included: "did you mean X?" when the fix is obvious
- One block per error: each error gets its own source snippet and message
- Error codes: E0001-style unique codes on every diagnostic (enables --explain flag and documentation later)
- Colored terminal output for errors (already decided in PROJECT.md)

### Token and AST Design
- Struct per node type: each AST node is its own C struct, linked via pointers (not tagged unions)
- Strings copied into arena: identifiers and literals are copied from source buffer into the arena allocator — source can be freed after parsing
- Iron_ prefix naming convention: Iron_IfStmt, Iron_FuncDecl, iron_parse_expr() — consistent with runtime naming
- Visitor pattern with callbacks: generic AST walk function that calls visitor struct methods per node type — reusable across semantic passes
- Arena allocator for all AST nodes (research recommendation, confirmed)

### Test File Format
- Separate .expected files: tests/hello.iron + tests/hello.expected for integration tests
- Error diagnostic tests check error code + line number only (not exact text) — less brittle, message wording can evolve
- C test harness: test runner built in C alongside the compiler — no external dependency
- Four test categories in Phase 1: lexer token tests, parser AST tests, error message tests, round-trip tests (parse then pretty-print)

### Project Scaffolding
- Follow implementation plan directory layout: src/lexer/, src/parser/, src/analyzer/, src/codegen/, src/runtime/, src/stdlib/, src/cli/ + tests/
- Move existing docs/ (language_definition.md, implementation_plan.md) into .planning/
- CMake + Ninja build system (research recommendation)
- Unity test framework for C unit tests (research recommendation)
- Defer CI (GitHub Actions) to Phase 3
- Include example .iron files (examples/hello.iron, examples/game.iron) as parser test inputs

### Claude's Discretion
- Exact arena allocator implementation details
- CMake configuration specifics
- Token enum ordering and internal structure
- Error message wording and suggestion heuristics
- AST visitor callback function signature design

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Language Specification
- `docs/language_definition.md` — Complete language syntax, types, keywords, operators, and grammar (to be moved to .planning/)
- `docs/implementation_plan.md` — Phase breakdown, token list, AST node types, grammar priorities, file structure (to be moved to .planning/)

### Research
- `.planning/research/STACK.md` — Recommends CMake+Ninja, Unity test framework, arena allocator, ASan/UBSan, hand-written parser
- `.planning/research/ARCHITECTURE.md` — Compiler pipeline architecture, data flow, component boundaries
- `.planning/research/PITFALLS.md` — Parser error cascade prevention, naming collision prefix, span tracking from day 1
- `.planning/research/FEATURES.md` — Table stakes features, error diagnostic standards

### Project Context
- `.planning/PROJECT.md` — Core value, constraints (C11, clang preferred, Rust-style diagnostics)
- `.planning/REQUIREMENTS.md` — LEX-01..04, PARSE-01..05, TEST-03 mapped to this phase

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- No existing code — greenfield project

### Established Patterns
- No patterns yet — Phase 1 establishes the foundational patterns (arena allocation, naming conventions, AST design) that all later phases build on

### Integration Points
- AST nodes produced here are consumed by Phase 2 (semantic analysis + codegen)
- Error diagnostic system established here is used by all later phases
- Arena allocator established here is used throughout the compiler
- Visitor pattern established here is used by semantic analysis passes

</code_context>

<specifics>
## Specific Ideas

- Error diagnostics should feel like Rust's — source snippets with arrows, colored output, "did you mean?" suggestions
- The implementation plan's token list (docs/implementation_plan.md, Phase 1) is the authoritative list of tokens to support
- The implementation plan's AST node types (docs/implementation_plan.md, Phase 2) are the authoritative node definitions
- Example .iron files from README.md should be parseable by end of Phase 1

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 01-frontend*
*Context gathered: 2025-03-25*
