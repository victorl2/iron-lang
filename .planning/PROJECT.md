# Iron

## What This Is

Iron is a compiled, strongly-typed programming language designed for game development. It compiles to C and produces standalone native binaries. The language prioritizes legibility, manual memory control (without a borrow checker), and built-in concurrency primitives — targeting developers who want the performance of C with a modern, readable syntax.

## Core Value

Every Iron language feature compiles to correct, working C code that produces a native binary — the compiler is the product, and correctness is non-negotiable.

## Requirements

### Validated

(None yet — ship to validate)

### Active

- [ ] Complete lexer that tokenizes all Iron syntax
- [ ] Recursive descent parser producing a full AST
- [ ] Semantic analysis: name resolution, type checking, escape analysis, concurrency checks
- [ ] C code generation for all language features
- [ ] Runtime library: strings, collections, ref counting, threading primitives
- [ ] Standard library modules: math, io, time, log
- [ ] CLI toolchain: iron build, iron run, iron check, iron fmt, iron test
- [ ] Compile-time evaluation (comptime)
- [ ] Raylib bindings as the game dev graphics backend
- [ ] Cross-platform: macOS, Linux, Windows
- [ ] Rust-style error diagnostics with source snippets and suggestions
- [ ] C unit tests for compiler internals + .iron integration tests for end-to-end

### Out of Scope

- Self-hosting (rewriting compiler in Iron) — stretch goal for a future milestone
- IDE/editor plugins — future milestone
- Package manager — future milestone
- Debugger integration — future milestone
- Other graphics backends (SDL, etc.) — raylib first, others later

## Context

- The compiler is written in C for self-containment and performance
- Iron compiles to C, then invokes clang (preferred) or gcc to produce the final binary
- Language spec is complete in `docs/language_definition.md`
- Implementation plan with phase breakdown exists in `docs/implementation_plan.md`
- Raylib is the target graphics library, dynamically linked as standard for game binaries
- No existing code yet — greenfield implementation

## Constraints

- **Language**: Compiler written in C (C11 standard)
- **C Backend**: clang preferred, gcc fallback — must support both
- **Platforms**: macOS, Linux, Windows from day one
- **Graphics**: Raylib as the game dev backend (dynamically linked)
- **Testing**: C unit tests (compiler internals) + .iron file integration tests (end-to-end)
- **Error UX**: Rust-style rich diagnostics — source snippets, arrows, suggestions
- **Standard**: Generated C must compile with `-std=c11 -Wall -Werror`

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Compiler in C | Self-contained, no bootstrap complexity, matches target output | -- Pending |
| Clang preferred, gcc fallback | Best cross-platform support for game dev, native on macOS | -- Pending |
| All 3 platforms from day one | Game dev needs Windows + macOS + Linux | -- Pending |
| Raylib as graphics target | Purpose-built for game dev, simple C API, cross-platform | -- Pending |
| Rust-style error diagnostics | Best developer experience for catching mistakes | -- Pending |
| Full 8-phase scope | Complete language implementation including comptime | -- Pending |

---
*Last updated: 2025-03-25 after initialization*
