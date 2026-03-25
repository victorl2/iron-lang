# Feature Research

**Domain:** Compiled programming language / compiler toolchain (game dev focus)
**Researched:** 2026-03-25
**Confidence:** HIGH (compiler toolchain landscape is well-understood; claims verified via official docs and established ecosystem comparisons)

---

## Feature Landscape

### Table Stakes (Users Expect These)

Features users assume exist. Missing these means the language feels incomplete or unusable regardless of how good everything else is.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Rich error diagnostics with source snippets | Rust set the bar: error messages must show the offending line, an arrow, and a suggestion. Anything less feels like a regression to C-era errors. | HIGH | Requires span tracking through the entire pipeline (lexer → parser → semantic → codegen). This is the single highest-leverage UX investment in the compiler. |
| Type inference for local variables | `val x = 10` is table stakes in any post-2010 compiled language. Requiring explicit types everywhere is a friction barrier. | MEDIUM | Iron already specifies this as `val`/`var` with inferred types. Implement type inference in the semantic analysis phase. |
| Null safety by default | After Kotlin, Swift, and Rust, developers expect the compiler to prevent null dereference at compile time. Optional types (`T?`) with narrowing are expected. | HIGH | Already in the spec. Requires nullable type tracking through the type checker and control-flow narrowing after `!= null` checks. |
| Working standard library (collections, I/O, strings) | Users expect `List`, `Map`, `String`, file I/O out of the box. A language without these requires FFI for every trivial task. | HIGH | Iron's spec covers this: `List[T]`, `Map[K,V]`, `Set[T]`, `io`, `time`, `log`. String methods on the type directly is the right call. |
| `build`, `run`, `check` CLI commands | Any modern language ships a unified CLI. Separate tools for compile/run/check are friction. Go, Zig, Rust all did this right. | LOW | Already in the spec as `iron build`, `iron run`, `iron check`. These delegate to the underlying compiler pipeline. |
| Code formatter | `gofmt` proved this: a canonical formatter eliminates style debates, reduces merge noise, and signals the language is production-minded. `zig fmt` and `rustfmt` followed. Any language without one in 2026 feels unfinished. | MEDIUM | `iron fmt` is in the spec. Parse to AST then pretty-print. AST-based approach guarantees formatting never changes semantics. |
| Test runner | `iron test` is expected. Developers should be able to write tests in the language itself without reaching for an external framework. | MEDIUM | In the spec. Needs a test discovery mechanism and a result reporter with pass/fail/count output. |
| Cross-platform support (macOS, Linux, Windows) | Game dev specifically requires all three from day one. A macOS-only or Linux-only language is not viable for game distribution. | HIGH | Already a constraint. C backend via clang/gcc handles most of this. Platform-specific concerns: path separators, threading APIs (pthreads vs Win32), dynamic library loading. |
| Generics | Without generics, collection types (List[T], Pool[T]) cannot be expressed in the language itself. Users will hit walls immediately. | HIGH | Full generics in the spec. C codegen must generate specialized structs per type instantiation (monomorphization). |
| Import / module system | One-file-one-module with `import` is table stakes. Users need to organize code across files. | MEDIUM | In the spec. Path-based module resolution. Must handle circular imports as a compile error. |
| `defer` for resource cleanup | Established pattern from Go and Zig for deterministic cleanup without RAII complexity. Game devs using raylib need this constantly (`defer close_window()`). | MEDIUM | In the spec. Codegen must collect defers and emit them at every scope exit, including before `return` statements. |
| Single-file output binary | A compiled language should produce a standalone executable. No VM, no runtime install required. | LOW | Already the architecture: Iron runtime statically linked, raylib dynamically linked. |

---

### Differentiators (Competitive Advantage)

Features that set Iron apart from C, Zig, Odin, and Jai. These are where Iron actually competes.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Explicit memory tiers without a borrow checker | `stack` (default) / `heap` / `rc` is a clean three-tier model. The borrow checker is the primary barrier to Rust adoption in game dev. Iron offers memory control at Rust's level without the cognitive overhead. Jai and Odin validate this market. | HIGH | Iron's escape analysis is the key: auto-free non-escaping heap allocations, warn on escaping without `free` or `leak`. This is novel in combination with explicit `rc` for shared ownership. |
| `comptime` call-site evaluation | Any pure function can run at compile time with no new syntax, separate file, or template DSL. This is cleaner than Zig's `comptime` blocks (which require the keyword at declaration sites too) and far cleaner than C++ templates. Baking shader source, lookup tables, and constants at zero runtime cost is directly valuable for game dev. | HIGH | Implemented as a mini-interpreter over the annotated AST. Run after semantic analysis, replace `comptime` nodes with computed literals. |
| `parallel` for loops with compile-time safety | `for i in range(n) parallel { ... }` with a compile error on outer-variable mutation is a better UX than raw `pthread` + manual partitioning. No equivalent in Jai or Odin. Zig does not have built-in parallel for. | HIGH | Compiler enforces no mutation of non-mutex outer variables in parallel body. Codegen splits range into chunks, submits to pool, implicit barrier. |
| First-class thread pools with named pools + CPU pinning | `pool("physics", 2)` with `pool.pin(2, 3)` is a practical, game-dev-specific concurrency primitive. Named pools map to real game architecture (render thread, physics thread, I/O thread). CPU pinning is critical for latency-sensitive game loops. No other language in this space ships this as a language primitive. | HIGH | Codegen wraps pthreads + a work queue. CPU affinity via `pthread_setaffinity_np` (Linux) and `thread_policy_set` (macOS). Windows requires `SetThreadAffinityMask`. |
| `mutex(val)` typed wrapper | A mutex that wraps a value and requires a lock closure to access it eliminates the "lock then forget to unlock" bug class. This is safer than Zig's raw `std.Thread.Mutex` and friendlier than Rust's `Mutex<T>`. | MEDIUM | Codegen wraps `pthread_mutex_t` with the value. `lock(func(var s) {...})` always pairs lock/unlock. |
| Raylib as a first-class import | `import raylib` works out of the box. Raylib has 70+ bindings but Iron's is integrated into the compiler's distribution, not a third-party package. For a game-dev language, a zero-setup graphics layer is a strong signal about the language's intent. | MEDIUM | Raylib is dynamically linked. The compiler ships the header bindings as part of its distribution. |
| `draw { }` game-loop block | `draw { clear(DARKGRAY); player.draw() }` auto-wraps `BeginDrawing()`/`EndDrawing()`. This is a small syntactic affordance that signals Iron is serious about game dev ergonomics, not just a general-purpose language with raylib bolted on. | LOW | Desugars to `BeginDrawing(); ...; EndDrawing()` in codegen. |
| `leak` as an explicit escape hatch | Rather than silently ignoring heap values that live forever (as in C), Iron requires an explicit `leak atlas` to silence the warning. This documents intent in code. No equivalent in Jai, Odin, or Zig. | LOW | Compiler diagnostic: missing `free` or `leak` on an escaping heap allocation is a warning, not silent. |
| Channels as a language primitive | `channel[T](n)` with `send`/`recv`/`try_recv`/`close` as first-class syntax (not stdlib calls) signals that concurrent data pipelines are idiomatic in Iron. This matches how game streaming/loading patterns are actually written. | MEDIUM | Codegen: ring buffer + mutex + condition variables. The type parameter means each channel is a generated struct. |

---

### Anti-Features (Commonly Requested, Often Problematic)

Features to deliberately NOT build, or defer explicitly.

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| Borrow checker / lifetime annotations | Rust's safety story is appealing. Developers want memory safety. | The borrow checker is the primary adoption barrier in Rust for game dev. It forces architectural rethinks (no mutable self + mutable children) that fight game-object graphs. Iron's escape analysis + explicit `heap`/`rc` provides enough guardrails without the full checker. This is a core design decision, not a gap. | Escape analysis warnings for unhandled heap escapes; `rc` for shared ownership; `leak` for intentional leaks. |
| Garbage collector | GC is familiar from C#/Unity, Go, Java. | GC pauses are incompatible with 60Hz game loops. Even generational GCs can spike. Deterministic cleanup (stack, defer, rc) is the right model for real-time systems. | Reference counting (`rc`) for shared ownership. Manual `heap`/`free` for everything else. Escape analysis for local heap. |
| Named arguments at call sites | Kotlin/Swift users expect `Player(pos: vec2(...), name: "Victor")`. | Named arguments create API lock-in: renaming a parameter becomes a breaking change. They also make positional reasoning harder for generated code. Iron's value is legibility through simple positional calls. | Enforce clear, ordered parameter names in function signatures. The struct layout is the documentation. |
| Operator overloading | `Vec2 + Vec2` is appealing for math-heavy game code. | Operator overloading obscures what code actually does. `+` on a complex type can mean anything from addition to concatenation to event dispatch. Iron's philosophy: "Legibility over magic." | Explicit function calls: `vec_add(a, b)`, `vec_scale(v, f)`. These are self-documenting and pattern-match well. |
| Implicit type conversions | Numeric coercions reduce verbosity. | Implicit conversions are a source of bugs (`Int` silently narrowing to `Int32`, `Float` losing precision to `Float32`). Iron requires explicit `Float(42)`, `Int(3.14)`. | Explicit cast functions that are cheap to write and obvious to read. |
| Package manager | Cargo/npm-like dependency management is expected by modern developers. | A package manager is a large surface area that distracts from compiler correctness. The ecosystem (third-party packages) doesn't exist yet. Building a resolver before there are packages to resolve is premature. | Defer to a future milestone. For now: the standard library + raylib covers game dev needs. C FFI is the escape hatch for everything else. |
| IDE plugins / editor extensions | VS Code integration is the expectation for any modern language. | Building a full LSP server is a 3-6 month project on its own, orthogonal to compiler correctness. Doing it badly (incomplete hover, broken completions) is worse than not having it. | Defer to post-v1. The `iron check` command gives editors a parsing/type-checking hook. A minimal LSP can be added after the compiler is stable. |
| Debugger integration (DAP) | Debug Adapter Protocol integration lets IDEs set breakpoints, inspect variables. | The generated C is an intermediate representation: source line mappings from Iron → C → binary are non-trivial. Building DAP integration before the language is stable is wrong ordering. | The `--verbose` flag showing generated C plus `assert` in the language covers initial debugging needs. Defer to post-v1. |
| Self-hosting compiler | Writing the Iron compiler in Iron is a milestone signal. | The compiler must be complete and stable before self-hosting. Attempting it early forces the language to have features the compiler needs (not the user), and creates a chicken-and-egg bootstrap problem. | Explicitly marked as a stretch goal / post-v1 milestone. Current C implementation is the right call. |
| Multiple inheritance | More powerful object hierarchy. | Multiple inheritance creates diamond inheritance problems, vtable complexity in C codegen, and mental model complexity for users. Single inheritance + multiple interface implementation covers all practical cases. | `object Player extends Entity implements Drawable, Updatable` — single inheritance + multiple interfaces. |
| Exceptions / try-catch | Familiar error handling from C++/Java/Python. | Exception mechanisms require stack unwinding infrastructure, which adds runtime overhead and complexity. The C backend makes this especially complex. Iron's multiple return values (`result, err = divide(...)`) is simpler and explicit. | Multiple return values. Return `T, Err?` and check at the call site. This is idiomatic in Go and Iron. |
| Variadic functions | Convenient for print-like functions. | Variadic functions require runtime type erasure or format strings. Iron's string interpolation (`"{name} has {hp} HP"`) covers 95% of the variadic use case without the complexity. | String interpolation handles the primary case. Built-in `print`/`println` are built-in, not user-definable variadic. |

---

## Feature Dependencies

```
[Rich Error Diagnostics]
    └──requires──> [Span tracking in Lexer/Parser]
                       └──requires──> [Source location on every AST node]
    └──requires──> [Semantic Analysis with typed AST]

[Type Inference]
    └──requires──> [Full Type Checker]
                       └──requires──> [Name Resolution]

[Null Safety / Nullable Narrowing]
    └──requires──> [Type Checker]
    └──requires──> [Control flow analysis (if/else narrowing)]

[Generics]
    └──requires──> [Type Checker]
    └──requires──> [C Codegen with monomorphization (per-type struct generation)]

[Comptime Evaluation]
    └──requires──> [Full Semantic Analysis (typed AST needed for interpretation)]
    └──requires──> [Parser (comptime expressions are first parsed)]

[Parallel For]
    └──requires──> [Concurrency Primitives (thread pool)]
    └──requires──> [Semantic Analysis (mutation analysis of parallel body)]

[Escape Analysis / Auto-Free]
    └──requires──> [Heap allocation tracking in Semantic Analysis]
    └──requires──> [Scope tree built during Name Resolution]

[`rc` Reference Counting]
    └──requires──> [Runtime Library (Iron_Rc_T struct)]
    └──requires──> [C Codegen (rc wrapping + release at scope exit)]

[Channels]
    └──requires──> [Runtime Library (ring buffer + mutex)]
    └──requires──> [Type Checker (channel[T] is a generic type)]

[Standard Library (math, io, time, log)]
    └──requires──> [Runtime Library (foundation C structs)]
    └──requires──> [Module system (import path resolution)]

[Code Formatter (iron fmt)]
    └──requires──> [Parser (AST must be lossless / preserve structure)]

[`iron test`]
    └──requires──> [Full compiler pipeline (build + run)]
    └──requires──> [Test discovery convention]

[Raylib bindings]
    └──requires──> [C Codegen (FFI call generation)]
    └──requires──> [Module system (import raylib)]
```

### Dependency Notes

- **Span tracking requires lexer/parser attention first:** Retrofitting source locations into AST nodes after the fact is painful. Build span tracking from Phase 1 (Lexer). Every token needs `line, col`. Every AST node needs a span. This is a Phase 1 decision that affects every later phase.
- **Generics require monomorphization in codegen:** Each unique instantiation (`Pool[Bullet]`, `Pool[Enemy]`) generates a separate C struct. The codegen must deduplicate (same instantiation used in two files should not emit the struct twice).
- **Comptime requires semantic analysis to complete first:** The comptime interpreter walks the typed AST, not the raw AST. Do not attempt comptime before types are resolved.
- **Escape analysis and scope tracking are tightly coupled:** Escape analysis (Phase 3c) is only correct if the scope tree (Phase 3a) is complete. These must be done in the right order within Phase 3.
- **Parallel for conflicts with outer mutation:** The semantic analysis must check that parallel for bodies do not mutate outer `var` bindings that are not wrapped in `mutex`. This is a correctness constraint, not an optimization.

---

## MVP Definition

### Launch With (v1 — Compiler Correctness Milestone)

The MVP is a compiler that produces correct native binaries for non-trivial game programs. Not feature-complete, but no wrong output.

- [ ] Lexer + Parser with full span tracking — foundation for diagnostics
- [ ] Semantic analysis: name resolution, type checking, nullable narrowing — prevents user errors
- [ ] C code generation for: variables, functions, objects, control flow, generics — core language
- [ ] Runtime library: strings, List, Map, Set, rc, threading primitives — standard data structures
- [ ] Standard library modules: math, io, time, log — covers 90% of non-graphics needs
- [ ] Raylib bindings — the game dev value proposition without these is unproven
- [ ] Rich error diagnostics with source snippets — table stakes for usability
- [ ] `iron build`, `iron run`, `iron check` CLI — basic workflow
- [ ] Escape analysis with warnings on unhandled heap escapes — memory safety story
- [ ] Cross-platform: macOS + Linux (Windows is important but can trail by one milestone)

### Add After Validation (v1.x)

- [ ] `iron fmt` — formatting is valuable but not correctness-blocking. Add once the parser is stable (re-parsing formatted output should produce the same AST)
- [ ] `iron test` — add once basic build/run works; the convention just needs a `test` function marker
- [ ] `comptime` evaluation — high value for game dev (embedded assets, lookup tables), but can be done after the core language is working
- [ ] Windows cross-platform parity — important for game distribution; can trail macOS/Linux by one milestone given the pthreads dependency

### Future Consideration (v2+)

- [ ] LSP / language server — high adoption value but 3-6 month investment. Defer until the compiler's diagnostic API is stable.
- [ ] Package manager — no ecosystem yet; premature to build a resolver.
- [ ] IDE plugins — downstream of LSP; defer.
- [ ] Debugger integration (DAP) — requires stable source map from Iron → C → binary. Defer until language is stable.
- [ ] Self-hosting — rewrite compiler in Iron. Stretch goal; requires the language to be feature-complete and stable.
- [ ] Additional graphics backends (SDL, Vulkan direct) — raylib first; validate game dev story before diversifying backends.

---

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| Rich error diagnostics | HIGH | HIGH | P1 — build from Phase 1 |
| Type inference + null safety | HIGH | MEDIUM | P1 — Phase 3 |
| Generics | HIGH | HIGH | P1 — required for collections |
| Collections (List, Map, Set) | HIGH | MEDIUM | P1 — runtime library |
| Raylib bindings | HIGH | MEDIUM | P1 — game dev identity |
| `defer` | HIGH | LOW | P1 — critical for raylib patterns |
| Escape analysis + auto-free | HIGH | HIGH | P1 — memory safety story |
| `rc` reference counting | HIGH | MEDIUM | P1 — shared textures/assets |
| `spawn`/`await`/`channel`/`mutex` | HIGH | HIGH | P1 — concurrency model |
| `parallel` for | HIGH | HIGH | P1 — game dev performance |
| `comptime` | HIGH | HIGH | P2 — game dev differentiator, post-core |
| `iron fmt` | MEDIUM | MEDIUM | P2 — after parser is stable |
| `iron test` | HIGH | LOW | P2 — after build/run works |
| Windows platform parity | HIGH | MEDIUM | P2 — after macOS/Linux |
| `draw { }` block | MEDIUM | LOW | P2 — ergonomic, not correctness |
| `leak` escape hatch | MEDIUM | LOW | P2 — part of escape analysis output |
| LSP server | HIGH | HIGH | P3 — post-v1 |
| Package manager | MEDIUM | HIGH | P3 — no ecosystem yet |
| Debugger integration | MEDIUM | HIGH | P3 — post-v1 |
| Self-hosting compiler | LOW | VERY HIGH | P3 — stretch goal |

**Priority key:**
- P1: Must have for launch
- P2: Should have, add when possible
- P3: Nice to have, future consideration

---

## Competitor Feature Analysis

| Feature | Jai | Odin | Zig | Iron (planned) |
|---------|-----|------|-----|----------------|
| Memory model | Manual, no borrow checker, no RC | Manual, allocator-based, no RC | Manual, allocator-based, no RC | stack/heap/rc explicit tiers + escape analysis |
| Compile-time execution | Any function with `#run` tag | Limited (`when` for static dispatch) | `comptime` keyword (declaration site) | `comptime` at call site — any pure function |
| Concurrency primitives | No built-in; relies on OS | No built-in; relies on OS stdlib | `std.Thread`, no parallel for | Named thread pools, `spawn`/`await`, typed channels, `parallel` for |
| Error messages | Good, source location | Good, source location | Excellent, Rust-style | Rust-style with snippets + suggestions (target) |
| Game dev focus | Yes (designed for it) | Yes (used for it) | General purpose | Yes (raylib first-class, `draw {}`, game patterns) |
| Formatter | Yes | Yes | `zig fmt` | `iron fmt` (planned) |
| Availability | Closed beta (invite-only as of 2026) | Open source | Open source | Open source (planned) |
| Null safety | No | No (`Maybe` type optional) | No (optional type, not enforced) | Yes, by default (`T?` with narrowing) |
| C interop | Excellent (transparent) | Excellent | Excellent | Via generated C (indirect; raylib bindings are first-class) |

Iron's differentiating bet is the combination of: null safety by default + explicit memory tiers without borrow checker + call-site `comptime` + named thread pools with parallel for. No single competitor has all four.

---

## Sources

- Rust Compiler Development Guide — Diagnostics: https://rustc-dev-guide.rust-lang.org/diagnostics.html (HIGH confidence — official Rust docs)
- "The Anatomy of Error Messages in Rust" — RustFest: https://rustfest.global/session/5-the-anatomy-of-error-messages-in-rust/ (MEDIUM confidence — community talk)
- Go blog on `gofmt`: https://go.dev/blog/gofmt (HIGH confidence — official Go blog)
- Zig guide — comptime: https://zig.guide/language-basics/comptime/ (HIGH confidence — official Zig docs)
- "What is Zig's Comptime?" — Loris Cro (Zig core team): https://kristoff.it/blog/what-is-zig-comptime/ (HIGH confidence — Zig core team author)
- Jai (programming language) — Wikipedia: https://en.wikipedia.org/wiki/Jai_(programming_language) (MEDIUM confidence — secondary source)
- "Jai, the game programming contender" (2025): https://bitshifters.cc/2025/04/28/jai.html (MEDIUM confidence — community analysis)
- Odin Programming Language overview: https://odin-lang.org/docs/overview/ (HIGH confidence — official docs)
- "Borrow checking, escape analysis, and the generational hypothesis" — Steve Klabnik: https://steveklabnik.com/writing/borrow-checking-escape-analysis-and-the-generational-hypothesis/ (MEDIUM confidence — expert author, not official spec)
- Raylib bindings list: https://github.com/raysan5/raylib/blob/master/BINDINGS.md (HIGH confidence — official raylib repo)
- Language Server Protocol overview: https://microsoft.github.io/language-server-protocol/ (HIGH confidence — official Microsoft/Eclipse docs)

---
*Feature research for: Iron compiled programming language*
*Researched: 2026-03-25*
