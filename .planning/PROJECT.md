# Iron

## What This Is

Iron is a compiled, strongly-typed programming language designed for game development. It compiles to C and produces standalone native binaries. The language prioritizes legibility, manual memory control (without a borrow checker), and built-in concurrency primitives — targeting developers who want the performance of C with a modern, readable syntax.

## Core Value

Every Iron language feature compiles to correct, working C code that produces a native binary — the compiler is the product, and correctness is non-negotiable.

## Current Milestone: v1.1 High IR

**Goal:** Introduce a decoupled, SSA-form High IR between AST and C emission, replacing the direct AST→C codegen with AST→IR→C. Prepares the compiler for a future Low IR / LLVM backend.

**Target features:**
- High IR data structures (SSA-form, basic blocks, typed instructions)
- AST → IR lowering pass for all Iron language features
- IR → C emission backend (replacing current direct codegen)
- Full replacement of old codegen — all programs compile through IR

## Requirements

### Validated

- ✓ Complete lexer that tokenizes all Iron syntax — v1.0
- ✓ Recursive descent parser producing a full AST — v1.0
- ✓ Semantic analysis: name resolution, type checking, escape analysis, concurrency checks — v1.0
- ✓ C code generation for all language features — v1.0
- ✓ Runtime library: strings, collections, ref counting, threading primitives — v1.0
- ✓ Standard library modules: math, io, time, log — v1.0
- ✓ CLI toolchain: iron build, iron run, iron check, iron fmt, iron test — v1.0
- ✓ Compile-time evaluation (comptime) — v1.0
- ✓ Raylib bindings as the game dev graphics backend — v1.0
- ✓ Cross-platform: macOS, Linux, Windows — v1.0
- ✓ Rust-style error diagnostics with source snippets and suggestions — v1.0
- ✓ C unit tests for compiler internals + .iron integration tests for end-to-end — v1.0

### Active

- [ ] High IR representation: SSA-form, basic blocks, typed instructions, C-agnostic
- [ ] AST → IR lowering pass covering all Iron language features
- [ ] IR → C emission backend producing equivalent output to old codegen
- [ ] Full replacement of old AST→C codegen path
- [ ] All existing integration tests pass through the new IR pipeline

### Out of Scope

- Self-hosting (rewriting compiler in Iron) — stretch goal for a future milestone
- IDE/editor plugins — future milestone
- Package manager — future milestone
- Debugger integration — future milestone
- Other graphics backends (SDL, etc.) — raylib first, others later
- Low IR / LLVM backend — future milestone, High IR designed to enable this
- IR optimization passes — future milestone, focus is on correct lowering first

## Context

- The compiler is written in C for self-containment and performance
- Iron compiles to C, then invokes clang (preferred) or gcc to produce the final binary
- Language spec is complete in `docs/language_definition.md`
- Raylib is the target graphics library, dynamically linked as standard for game binaries
- v1.0 shipped: full compiler pipeline (lexer→parser→semantic→codegen), runtime, stdlib, CLI, comptime, raylib, cross-platform
- Current codegen emits C directly from AST traversal (gen_stmts.c, gen_exprs.c, etc.)
- Future architecture: source → AST → High IR → Low IR (LLVM) → bytecode/C

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
| Full 8-phase scope | Complete language implementation including comptime | ✓ Good |
| High IR before LLVM | Decoupled IR enables backend swap without rewriting lowering logic | -- Pending |
| SSA form for IR | Enables future optimization passes and maps cleanly to LLVM IR | -- Pending |
| Full codegen replacement | No dual-path maintenance burden; clean architectural break | -- Pending |

---
*Last updated: 2026-03-27 after milestone v1.1 start*
