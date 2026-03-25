# Project Research Summary

**Project:** Iron — compiled programming language (transpile-to-C)
**Domain:** Compiler toolchain / game development language
**Researched:** 2026-03-25
**Confidence:** HIGH

## Executive Summary

Iron is a statically typed, compiled language that transpiles to C11, targeting game development with raylib as a first-class graphics backend. Research confirms that building this class of compiler in 2026 is well-understood: the correct architecture is a multi-pass pipeline (lexer → parser → semantic analysis → comptime evaluation → C codegen → clang/gcc invocation), implemented in C11 with hand-written recursive descent parsing, an annotated AST as the shared IR, and an arena allocator for all AST memory. The dominant reference implementations (Nim's C backend, Zig's bootstrap compiler, Clang itself) all converge on this pattern, making the architectural choices here high-confidence.

Iron's competitive position rests on a specific combination no competitor offers: null safety by default, explicit memory tiers (stack/heap/rc) without a borrow checker, call-site `comptime` for zero-cost game constants, and named thread pools with `parallel for` as language primitives. Jai and Odin validate the market for a game-focused language without Rust's borrow checker, but neither has null safety nor built-in parallel-for safety checks. Iron's bet is that game developers will trade GC and borrow-checking for explicit, safe-enough memory control and better concurrency ergonomics.

The principal risks are not architectural but implementation-sequencing. Several pitfalls — generated C naming collisions, defer behavior at early returns, inheritance struct layout, and comptime step limits — are cheap to prevent from the start and expensive to retrofit later. The research is clear: establish the symbol prefix convention and struct layout strategy in the first codegen commit, implement defer with full exit-point traversal from the start, and add a comptime step limit before writing any evaluation tests. Windows threading compatibility requires a platform abstraction layer before any pthread code is written.

## Key Findings

### Recommended Stack

The compiler is written in C11 using CMake 3.25+ and Ninja as the build system. This combination handles all three target platforms (macOS, Linux, Windows) from day one — a plain Makefile becomes a liability on Windows immediately. The primary testing strategy is Unity (ThrowTheSwitch) v2.6.1 for compiler-internal unit tests and a shell-based integration runner for end-to-end `.iron` → compile → run → diff tests. All builds in CI run with `-fsanitize=address,undefined`; a separate CI pass runs ThreadSanitizer on concurrency tests. Valgrind is explicitly ruled out — it does not work reliably on Apple Silicon and ASan catches everything Valgrind catches and more.

Internal data structures use `stb_ds.h` (single-header, public domain) for symbol tables and hash maps, and a custom ~100-line arena allocator for all AST nodes and tokens. Raylib 5.5 is fetched and built from source via CMake FetchContent, pinned to the exact tag — this ensures deterministic behavior across all three platforms and eliminates system-version mismatch.

**Core technologies:**
- C11: Compiler implementation language — `_Generic`, `static_assert`, `stdint.h` all available; mandated by project
- CMake 3.25+ / Ninja: Cross-platform build — handles Windows SDK paths, macOS frameworks, Ninja gives 10-50x faster incremental builds
- clang 18+ (gcc fallback): Backend C compiler — superior error messages, ships as macOS system compiler, ASan built in
- raylib 5.5 (FetchContent): Graphics backend — pure C99 API, zlib license allows static linking, no external dependencies
- Unity v2.6.1: Unit testing — two-header zero-friction embed; works with GCC, Clang, MSVC
- stb_ds.h: Symbol tables and hash maps — type-safe, public domain, single-header, no compilation step
- Arena allocator (custom): AST memory — one malloc per compilation unit, free at the end; standard pattern in Clang, TCC, Zig bootstrap
- ASan + UBSan: Memory debugging — `-fsanitize=address,undefined`; replaces Valgrind entirely on all platforms

### Expected Features

The compiler must ship with a complete pipeline, rich error diagnostics, and raylib bindings to validate Iron's game-dev identity. The diagnostics requirement is non-negotiable: Rust set the bar and any post-2010 compiled language without source snippets + arrows + suggestions feels like a regression. Span tracking must be built into the lexer from Phase 1 — retrofitting source locations onto AST nodes after the fact is expensive and error-prone.

**Must have (table stakes — v1):**
- Rich error diagnostics with source snippets — users will not adopt without Rust-quality errors
- Type inference (`val`/`var`) and null safety by default — table stakes in any post-2010 language
- Generics with monomorphization — required to implement `List[T]`, `Map[K,V]` in the language itself
- Working standard library: `List`, `Map`, `Set`, strings, `io`, `math`, `time`, `log`
- Raylib bindings (`import raylib`) — the game dev value proposition is unproven without it
- `defer` for resource cleanup — game devs need `defer close_window()` constantly
- `heap`/`rc` explicit memory tiers with escape analysis — the memory safety story
- `spawn`/`await`/`channel`/`mutex` / `parallel for` — the concurrency model
- `iron build`, `iron run`, `iron check` CLI
- Cross-platform: macOS + Linux (Windows can trail one milestone)

**Should have (v1.x differentiators):**
- `comptime` call-site evaluation — high value for embedded shaders, lookup tables; defer until core is working
- `iron fmt` — add once parser is stable
- `iron test` — add once build/run works
- Windows parity — important for game distribution; defer from initial macOS/Linux milestone
- `draw {}` game-loop block — small ergonomic affordance, low cost, high signal for game dev intent

**Defer (v2+):**
- LSP / language server — 3-6 month investment; wait until compiler diagnostic API is stable
- Package manager — no ecosystem yet; premature to build a resolver
- Debugger integration (DAP) — requires stable source map from Iron → C → binary
- Self-hosting compiler — stretch goal; language must be feature-complete first

### Architecture Approach

The correct architecture for a transpile-to-C compiler is a strict multi-pass pipeline where each phase runs to completion, collects all errors within its domain, and gates later phases on clean output. The annotated AST is the single shared intermediate representation — each pass adds fields to existing nodes (resolved types, escape flags, comptime-evaluated literals) rather than producing a new data structure. This avoids designing and maintaining a separate IR and is the pattern used by Nim's C backend. Codegen walks the final annotated AST and emits C11 text; it should never encounter unresolved nodes.

**Major components:**
1. Lexer — tokenize source, track `(file, line, col)` per token; span tracking is a Phase 1 decision that cannot be retrofitted
2. Parser — recursive descent, builds unresolved AST with error recovery; `ErrorNode` in AST allows continued parsing after syntax errors
3. Semantic Analysis — four sub-passes in strict order: name resolution → type checking → escape analysis → concurrency checks
4. Comptime Evaluator — mini interpreter over fully-typed AST; replaces `comptime` nodes with literal values; isolated phase after semantic analysis
5. C Code Generator — walks annotated AST, emits C11; split across `types.c`, `stmts.c`, `exprs.c` for manageability
6. Runtime Library (`iron_runtime.c`) — always statically linked; String, List, Map, Set, Rc, threads; compiled into every binary
7. Standard Library modules — only linked when imported; `iron_math`, `iron_io`, `iron_time`, `iron_log`
8. CLI Driver — orchestrates the pipeline; invokes clang/gcc via subprocess; detects available compiler at startup

**Key patterns:**
- Multi-pass with strict error gating: if any phase has errors, later phases do not run
- Annotated AST as IR (no separate lowering to machine IR): correct for a C-targeting compiler
- Arena allocation for all AST nodes: one free after codegen, not thousands of individual frees
- Topological sort before struct emission: C requires types defined before use; file-visit order is incorrect
- Defer via scope-exit stack: every `return`/`break`/`continue` drains the defer stack for all enclosing scopes

### Critical Pitfalls

1. **Generated C naming collisions** — Establish `ir_` prefix for user symbols and `Iron_` for runtime from the first codegen commit. Maintain a C reserved-word blocklist. Emit Iron-level errors on collision, not C-level errors. Never retrofit — the recovery cost is HIGH.

2. **Defer failing at early returns** — Implement defer as a scope-exit stack traversed at every `return`/`break`/`continue`, not as tail-emission at function end. The naive approach leaks resources on all non-happy-path exits. Test: function with defer + 3 early returns; ASAN must show zero leaks on all paths.

3. **Inheritance struct layout breaking polymorphic casts** — Embed parent struct as first field (`typedef struct { Entity _base; ... } Player;`), not field flattening. Embed a type tag in the root base type from the start. Retrofitting struct layouts after inheritance codegen ships requires rewriting everything.

4. **Comptime interpreter with no step limit** — Implement a step counter (1,000,000 steps default) before writing the first comptime test. A missing limit lets `comptime fibonacci(50)` hang the compiler indefinitely. This is labeled "optional" in some planning docs — it is not optional.

5. **Lambda closures capturing stack variables that escape** — Apply escape analysis to closure captures. If a lambda is stored in an object field or passed to `spawn`, captured stack locals must be promoted to heap or copied into the environment struct. Test: lambda stored in object field, triggered after declaring scope exits; ASAN must be clean.

6. **Windows pthread compatibility** — Build a platform threading abstraction (`iron_thread_platform.h`) before any runtime threading code is written. `pthread_setaffinity_np` does not exist on Windows. `pool.pin()` must no-op on unsupported platforms, not fail to compile.

7. **Parser error cascade** — Represent errors as `ErrorNode` in AST; do not stop parsing on first error. Define synchronization tokens (`func`, `object`, `interface`, `enum`, `import`). A file with 3 independent syntax errors must produce exactly 3 error messages, not 3 + N cascades.

## Implications for Roadmap

The architecture research directly maps to an 8-phase build order. Each phase has strict dependencies on prior phases; the ordering below follows the component dependency graph confirmed by the architecture research.

### Phase 1: Foundation and Lexer
**Rationale:** Everything in the compiler depends on tokens and source spans. Span tracking (`file, line, col` on every token) must be a Phase 1 decision — it cannot be retrofitted. The utility layer (arena allocator, string builder, dynamic array) has no dependencies and unblocks everything else.
**Delivers:** A complete tokenizer that handles all Iron token types with full source location tracking; arena allocator; strbuf; Unity test harness wired to CI with ASan+UBSan
**Addresses:** Rich error diagnostics (depends on span tracking from Phase 1), `iron check` command foundation
**Avoids:** Pitfall 13 (parser error cascade — span tracking enables good recovery); the "retrofitting spans" anti-pattern

### Phase 2: Parser and AST
**Rationale:** The AST is the data structure shared by every later phase. Its node types, the span it carries, and the `ErrorNode` recovery approach must be locked in here. The recursive descent parser is the only correct choice — parser generators produce inferior error messages and recovery.
**Delivers:** Complete AST node definitions with source spans on every node; recursive descent parser with error recovery and `ErrorNode` AST insertion; parser-level error recovery with synchronization tokens
**Avoids:** Pitfall 13 (implement error recovery during Phase 2, not after); Anti-Pattern 1 (interleaving parsing and semantic analysis)
**Research flag:** Standard patterns — recursive descent is extremely well-documented; skip research-phase

### Phase 3: Semantic Analysis
**Rationale:** Semantic analysis has four sub-passes that must run in strict order. Name resolution must complete before type checking; type checking must complete before escape analysis; escape analysis before concurrency checks. Each sub-pass adds annotations to existing AST nodes.
**Delivers:** Name resolution with scope tree; type checker with bidirectional type propagation and type-origin metadata for error attribution; escape analysis for heap/stack tracking; concurrency checks for parallel-for mutation and spawn captures; nullable type narrowing
**Addresses:** Type inference, null safety, escape analysis memory safety story
**Avoids:** Pitfall 4 (escape analysis over-approximation — document precision contract before writing code); Pitfall 6 (parallel for mutation check — conservatively flag method calls on captured vars, not just direct assignments); Pitfall 10 (type inference error attribution — store type-origin metadata from the start)
**Research flag:** Needs per-sub-pass design before implementation — escape analysis precision contract and concurrency check scope need explicit design documents before coding

### Phase 4: C Code Generator
**Rationale:** Codegen can only begin after a complete, clean annotated AST exists. The codegen must establish the symbol prefix convention (`ir_` / `Iron_`) and struct layout strategy in its very first commit — both have HIGH recovery cost if retrofitted.
**Delivers:** Full C11 emission for variables, functions, control flow, generics (monomorphization), inheritance (embedded base struct with type tag), defer (scope-exit stack), interfaces (vtable structs), string interpolation, nullable optionals; topological sort for struct emission order; forward declarations emitted before struct bodies
**Addresses:** Generics, `defer`, inheritance, interface dispatch, nullable `T?`
**Avoids:** Pitfall 1 (naming collisions — establish prefix scheme in commit 1); Pitfall 2 (forward declarations — topological sort before first multi-file test); Pitfall 3 (defer at early returns — implement exit-point traversal from the start); Pitfall 8 (inheritance layout — embedded base struct + type tag, designed before any inheritance codegen); Anti-Pattern 2 (never generate C directly from parser)
**Research flag:** Defer implementation and monomorphization deduplication are tricky — worth a brief design pass before Phase 4d and 4f

### Phase 5: Runtime Library
**Rationale:** The runtime is independent of the compiler front-end and can be developed concurrently with codegen, but must exist before any end-to-end integration tests can run. The platform threading abstraction must be the first thing built in this phase.
**Delivers:** `iron_runtime.c`: Iron_String (UTF-8 + codepoint_count), List, Map, Set, Iron_Rc (retain/release), Optional; `iron_threads.c`: platform-abstracted thread pool, named pools, channel (ring buffer + mutex + condvar), mutex wrapper; pthreads on POSIX, C11 threads / tinycthread on Windows
**Addresses:** Collections, `rc` reference counting, channels, mutex wrapper, cross-platform threading
**Avoids:** Pitfall 5 (rc cycles — document limitation prominently; add structural cycle detection in codegen; debug-build cycle detection in runtime); Pitfall 11 (Windows threading — platform abstraction layer before any pthread code); Pitfall 12 (string unicode — define `Iron_String` struct layout before any string ops; test with Japanese/emoji from day one)
**Research flag:** Windows threading abstraction (C11 threads vs tinycthread vs Win32 API) needs a brief spike before implementation

### Phase 6: Standard Library Modules
**Rationale:** Stdlib modules depend on the runtime and must be importable via the module system. These are Iron-level wrappers around the runtime's C primitives.
**Delivers:** `iron_math`, `iron_io`, `iron_time`, `iron_log` as importable modules; module system path resolution; circular import detection as a compile error
**Addresses:** Standard library (table stakes), `import` system
**Avoids:** Circular import compile errors (must be caught here, not silently produce broken code)
**Research flag:** Standard patterns — skip research-phase

### Phase 7: CLI Toolchain and Raylib Bindings
**Rationale:** With a working compiler pipeline and runtime, the CLI and raylib bindings can be completed. These two concerns are grouped because raylib bindings require the full pipeline to be testable end-to-end, and the CLI is what makes the language usable.
**Delivers:** `iron build`, `iron run`, `iron check` with cross-platform clang/gcc detection; raylib 5.5 bindings as `import raylib`; `draw {}` block desugaring; rich diagnostic formatting with source snippets, arrows, suggestions; cross-platform macOS + Linux verified
**Addresses:** Raylib (the game dev identity), CLI (table stakes), `draw {}`, rich diagnostics
**Avoids:** Pitfall integration gotchas: clang vs gcc detection, raylib struct adapter generation (never rely on Iron struct layout matching raylib layout by accident)
**Research flag:** Raylib binding generation strategy (whether to hand-write or generate from raylib headers) warrants a brief design decision before implementation

### Phase 8: Comptime Evaluator and Developer Tools
**Rationale:** Comptime is a differentiator but requires a complete, fully-typed AST to be correct. It is the last compiler phase to implement. Developer tools (`iron fmt`, `iron test`) are added once the pipeline is stable.
**Delivers:** Comptime evaluator (mini AST interpreter with 1,000,000-step limit, configurable via `--comptime-limit`); path resolution relative to source file (not cwd); `iron fmt` (lossless AST pretty-printer); `iron test` (test discovery + result reporter); Windows platform parity
**Addresses:** `comptime` (game dev differentiator), `iron fmt`, `iron test`, Windows parity
**Avoids:** Pitfall 7 (comptime divergence — step limit is day-one, not optional); Pitfall 14 (comptime read_file path resolution — source-relative paths specified before implementation)
**Research flag:** Comptime interpreter design (recursive AST evaluation vs bytecode) warrants a design decision before Phase 8 — recursive evaluation is simpler to build first per Clang's experience

### Phase Ordering Rationale

- Phases 1-4 are strictly sequential: each depends on the output of the previous
- Phase 5 (runtime) can overlap with Phase 4 (codegen) — they are independent; runtime is needed before end-to-end tests
- Phase 6 depends on Phase 5 (runtime) and Phase 3 (module system in name resolution)
- Phase 7 depends on the full pipeline (Phases 1-6) being functional
- Phase 8 depends on semantic analysis being complete (comptime needs typed AST) and parser being stable (fmt needs lossless parse)
- Pitfall prevention is ordered correctly: naming conventions in Phase 4 commit 1, platform abstraction in Phase 5 commit 1, step limit in Phase 8 commit 1

### Research Flags

Phases needing deeper research / design before implementation:
- **Phase 3 (Semantic Analysis):** Escape analysis precision contract and concurrency mutation check scope should be written as design documents before coding begins. The precision boundary (intra-procedural only, with documented limitations) must be decided upfront.
- **Phase 4 (C Code Generator):** Defer with early returns and monomorphization deduplication are edge-case-heavy. A brief design pass before Phase 4d (defer) and Phase 4f (generics) is warranted.
- **Phase 5 (Runtime Library):** Windows threading approach (C11 `<threads.h>` vs tinycthread vs Win32 shim) needs a spike to confirm the right abstraction before writing `iron_threads.c`.
- **Phase 7 (Raylib Bindings):** The binding generation strategy (hand-written vs header-generated) needs a decision before implementation.
- **Phase 8 (Comptime Evaluator):** Recursive AST evaluation vs bytecode interpreter — research confirms recursive is correct for first implementation, but this should be confirmed against Iron's specific comptime requirements.

Phases with standard, well-documented patterns (skip research-phase):
- **Phase 1 (Lexer):** Tokenizers are extremely well-understood; Unity + ASan setup is mechanical.
- **Phase 2 (Parser):** Recursive descent is comprehensively documented; Clang and GCC are reference implementations.
- **Phase 6 (Stdlib):** C standard library wrappers follow obvious patterns; no novel design required.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All core technology choices verified against official docs and community consensus. raylib 5.5 version confirmed. Unity v2.6.1 confirmed. CMake + Ninja cross-platform rationale verified. |
| Features | HIGH | Feature table stakes and differentiators verified against official docs for Zig, Odin, Go, Rust. Competitor analysis based on official language docs. MVP boundary is well-reasoned. |
| Architecture | HIGH | Multi-pass pipeline, annotated AST as IR, and recursive descent confirmed by Nim compiler internals docs, Zig self-hosted compiler architecture, and Clang design docs. All major patterns sourced from production compilers. |
| Pitfalls | HIGH | Pitfalls derived from compiler internals docs, production compiler post-mortems, and Iron-specific spec analysis. Prevention strategies verified against known working implementations. |

**Overall confidence:** HIGH

### Gaps to Address

- **Escape analysis interprocedural precision:** The initial escape analysis is intentionally intra-procedural only. The boundary between "warn" and "auto-free" for function call arguments needs explicit specification before Phase 3c. This is not a gap in research but a design decision that must be made explicit.

- **`rc` cycle detection strategy:** Research confirms rc cycles are a fundamental limitation. The `weak` reference pattern is the canonical solution but is not in the current Iron spec. Before Phase 5, decide whether to add `weak T` to the language or rely on documentation + debug-mode cycle detection.

- **Generics monomorphization deduplication:** When the same generic instantiation (`List[Enemy]`) appears across multiple source files, the codegen must not emit the generated struct twice. The deduplication mechanism (global instantiation registry vs post-processing) needs to be decided before Phase 4 generics implementation.

- **Windows CI setup timing:** The research recommends Windows CI from Phase 5 onward. The practical question of which CI provider and Windows image to use is out of scope for research but must be decided before Phase 5 begins.

## Sources

### Primary (HIGH confidence)
- [Nim Compiler Internals](https://nim-lang.org/docs/intern.html) — multi-pass architecture, AST as IR, C backend
- [Zig Self-Hosted Compiler Architecture](https://kristoff.it/blog/zig-self-hosted-now-what/) — comptime as separate evaluator phase
- [Zig comptime documentation](https://zig.guide/language-basics/comptime/) — comptime approach confirmed
- [AddressSanitizer clang docs](https://clang.llvm.org/docs/AddressSanitizer.html) — flags and platform support
- [raylib 5.5 release](https://github.com/raysan5/raylib/releases/tag/5.5) — version confirmed Nov 18, 2024
- [Unity v2.6.1](https://github.com/ThrowTheSwitch/Unity) — version confirmed Jan 1, 2025
- [Odin Language overview](https://odin-lang.org/docs/overview/) — competitor feature analysis
- [Go blog on gofmt](https://go.dev/blog/gofmt) — formatter as table stakes
- [Rust Compiler Development Guide — Diagnostics](https://rustc-dev-guide.rust-lang.org/diagnostics.html) — diagnostic design confirmed
- [SEI CERT C reserved identifiers](https://wiki.sei.cmu.edu/confluence/display/c/DCL37-C.+Do+not+declare+or+define+a+reserved+identifier) — C naming collision risks

### Secondary (MEDIUM confidence)
- [chibicc — small C compiler reference](https://github.com/rui314/chibicc) — arena allocator and multi-pass patterns
- [ASan vs Valgrind — Red Hat](https://developers.redhat.com/blog/2021/05/05/memory-error-checking-in-c-and-c-comparing-sanitizers-and-valgrind) — Valgrind macOS deprecation
- [Zig comptime analysis — matklad](https://matklad.github.io/2025/04/19/things-zig-comptime-wont-do.html) — comptime step limit pitfall
- [Defer implementation edge cases](https://dev.to/lerno/implementing-defer-3l76) — defer at early returns
- [Resilient recursive descent parsing](https://thunderseethe.dev/posts/parser-base/) — error recovery patterns
- [TinyCThread](https://tinycthread.github.io/) — cross-platform threading fallback
- [Cyclic reference counting ISMM 2024](https://dl.acm.org/doi/10.1145/3652024.3665507) — rc cycle limitations confirmed
- [Monomorphization code bloat](https://lobste.rs/s/aar0zx/dark_side_inlining_monomorphization) — generic instantiation explosion risk

---
*Research completed: 2026-03-25*
*Ready for roadmap: yes*
