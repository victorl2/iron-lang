# Pitfalls Research

**Domain:** Compiled programming language / transpile-to-C compiler
**Researched:** 2026-03-25
**Confidence:** HIGH (compiler internals from official sources and production compiler post-mortems; Iron-specific risks from language spec analysis)

---

## Critical Pitfalls

### Pitfall 1: Generated C Naming Collisions

**What goes wrong:**
Iron identifiers collide with C reserved keywords, POSIX-reserved names, or other generated symbols. A user writes `object Signal { }` and the generated C emits `typedef struct { ... } Signal;` — but `Signal` is POSIX-reserved on some systems. Or a method `Player.new` generates `Player_new` which collides with a runtime function named `Iron_Player_new`. The generated `.c` file fails to compile under `-Wall -Werror` with cryptic errors far from the Iron source.

**Why it happens:**
Iron has a richer identifier space than C. Dots in method names (`Player.update`), generic instantiations (`List[Enemy]`), and flat symbol emission without a consistent prefix scheme all produce collisions. Developers build the codegen incrementally and only discover collisions when they introduce a conflicting user-defined name late in development.

**How to avoid:**
Adopt a single mandatory prefix for all generated symbols from day one: `Iron_` for runtime/stdlib, `ir_` for user-declared types, `ir_` + type + `__` + method for methods (e.g., `ir_Player__update`). Never emit a user symbol without a prefix. Maintain an explicit blocklist of C reserved words (`int`, `long`, `register`, `restrict`, `_Bool`, `_Complex`, `__*`) and POSIX-reserved suffixes (`_t`). Fail with an Iron-level error — not a C-level error — if a user identifier would produce a collision.

**Warning signs:**
- Codegen tests fail with "redefinition of..." or "conflicting types" errors in C
- Users naming objects with common English words (File, Error, String, List) break builds
- Segfaults under certain identifier combinations on specific platforms

**Phase to address:** Phase 4 (C Code Generation) — establish naming conventions in the very first codegen commit. Retrofitting is expensive.

---

### Pitfall 2: Missing Forward Declarations / Topological Sort Failure

**What goes wrong:**
Iron allows `Player extends Entity` and `Enemy extends Entity` across files. The C codegen emits struct definitions in file-visit order. If `player.iron` is processed before `entity.iron`, the generated C contains `typedef struct { Entity base; ... } Player;` before `Entity` is defined. The C compiler rejects this. Cyclic struct dependencies (A contains B which contains A) cause a complete topological sort failure.

**Why it happens:**
The implementation plan specifies topological sort for struct definitions (Phase 4b), but the details matter. Developers prototype codegen in file order first and defer sorting "until later," which never comes. Mutual references between objects (two objects that hold references to each other by pointer) are legitimate in Iron but create apparent cycles that naive topological sort treats as errors.

**How to avoid:**
Emit generation in this mandatory order: (1) forward declarations for all structs, (2) topologically sorted struct body definitions, (3) function prototypes, (4) function bodies. Implement cycle detection in the sort — a cycle in value-type fields is a genuine error (infinite size struct); a cycle through pointer fields is legal and handled by forward declarations. The semantic analysis phase should detect and reject value-type cycles before codegen runs.

**Warning signs:**
- Codegen test with two mutually-referencing objects fails to compile
- Any test file that imports another file and uses the imported type as a field (not pointer) fails
- Compiler processes files alphabetically and tests pass; reordering files breaks tests

**Phase to address:** Phase 4 (C Code Generation) — topological sort must be implemented before any multi-object tests are added, not after.

---

### Pitfall 3: Defer Codegen Fails at Early Returns

**What goes wrong:**
The implementation plan shows defer emitting cleanup at scope exit. The naive implementation only handles the happy path: defers are emitted at the closing `}` of a function. Early `return` statements inside the function silently skip all defers. This produces real resource leaks in generated code that pass unit tests (which test the happy path) but fail under error conditions.

**Why it happens:**
The single-exit-point assumption is natural when first sketching codegen. The implementation plan's example only shows the simple case. Nested scopes with multiple returns, `break` inside a `while` loop that has a defer, and labeled control flow all require systematic defer unwinding at every exit point — not just the function end.

**How to avoid:**
Model defers as a scope stack. Every time a `return`, `break`, or `continue` is encountered during codegen, iterate the defer stack from the current depth back to the appropriate scope boundary and emit all defers in reverse order before the exit statement. Test this explicitly: write an Iron function with a defer and three early returns, verify all three execution paths call the deferred code. Do not implement defer as simple tail-emission.

**Warning signs:**
- Defer tests only cover single-return functions
- File handles or memory are not freed when a function returns early due to an error
- `defer close_window(window)` in `main` works, but `defer close_file(f)` inside a conditional block with an early return leaks

**Phase to address:** Phase 4d (Defer Implementation). Explicit test: function with defer + early return must emit deferred code on every path.

---

### Pitfall 4: Escape Analysis Over-Approximation Causes Missed Auto-Frees

**What goes wrong:**
The escape analysis is supposed to detect heap values that don't escape their scope and auto-free them. A conservative (over-approximating) implementation marks too many values as "escaping" — values stored in a local array, passed to a function that doesn't retain them, or returned only through one arm of a conditional. The result: the Iron compiler warns about potential leaks for code the programmer knows is correct, training users to ignore warnings. Actual leaks are buried in the noise.

**Why it happens:**
Escape analysis is undecidable in the general case. Developers implement a simple syntactic check (does this value cross a scope boundary in the AST?) rather than a flow-sensitive check. Function calls are conservatively marked as "might escape" because the callee's behavior is unknown at the call site without interprocedural analysis.

**How to avoid:**
Scope the initial escape analysis to the intra-procedural case: track whether a heap value is returned or stored in an outer-scope variable within the same function. Mark cross-function passing as "unknown" (no auto-free, but also no false-positive warning unless the value has no `free`/`leak` and leaves the declaring function scope). Document the analysis precision level. Add interprocedural analysis as an explicit later enhancement. Never emit a warning for a pattern the user cannot fix without `leak`.

**Warning signs:**
- Users `leak` values to silence warnings even though those values are genuinely local
- Auto-free tests require `valgrind` to verify — write them from the start
- Escape analysis takes quadratic time on large functions (sign of naive implementation)

**Phase to address:** Phase 3c (Escape Analysis). Define the precision contract in the implementation before writing a line of code.

---

### Pitfall 5: Reference Counting Cycles Leak Memory Silently

**What goes wrong:**
Iron's `rc` type uses reference counting. Game objects commonly form cycles: `Scene` holds a list of `Entity` items, each `Entity` holds a back-reference to its `Scene` via `rc Scene`. When the scene is dropped, its rc count never reaches zero because each entity holds a reference to it. The leak is silent — no crash, no warning, just growing memory usage across scene transitions. Valgrind reports leaks but the source is unclear.

**Why it happens:**
Reference counting cannot reclaim cycles. This is a fundamental limitation, not an implementation bug. The language spec does not mention this constraint. If users are not warned at language-design time, they will naturally create cycles (common in game entity-scene graphs, doubly-linked structures, UI trees).

**How to avoid:**
Document the `rc` cycle limitation prominently in the language spec and compiler diagnostics. Provide `weak` references or advise on ownership hierarchies (parent owns child via `rc`, child holds parent via raw reference or pointer) as the canonical pattern for tree structures. The runtime's `Iron_Rc` implementation should include cycle detection in debug builds using a periodic mark-and-sweep (acceptable compile-time flag, not enabled in release). Emit a compiler warning when an `rc T` field on type `A` transitively contains an `rc A` — this is a structural cycle indicator detectable at compile time.

**Warning signs:**
- Memory grows monotonically across scene loads/unloads in a game
- Valgrind reports leaks in `Iron_Rc_release` chains
- Users with entity-scene graphs are the first reporters of leaks

**Phase to address:** Phase 5 (Runtime Library) for the runtime implementation; Phase 3 documentation for the language contract; Phase 4 for the optional cycle warning in codegen.

---

### Pitfall 6: Concurrency Data Race in Parallel For — Incorrect Mutation Check

**What goes wrong:**
The spec states `parallel for` bodies cannot mutate outer non-mutex variables. The semantic analysis check is syntactic: it looks for direct assignments to outer-scope identifiers. But mutation through method calls (`particles[i].update(dt)` where `update` mutates fields on a shared object) passes the check. Race conditions occur at runtime — not caught at compile time — and produce intermittent incorrect behavior that is extremely hard to debug.

**Why it happens:**
Phase 3d's concurrency check is underspecified. The "cannot mutate outer variables" rule is clear for simple assignments but undefined for method calls, field access through references, and closure captures. Implementing only the easy case leaves the hard case silently broken.

**How to avoid:**
For Phase 3d, define the check conservatively: inside `parallel for`, any call to a method with a `var` receiver that is or captures an outer-scope variable is a compile error. Access to outer immutable (`val`) variables is fine. Access to `mutex(T)` variables is fine. All other outer variable references through method calls must be flagged. Test with the canonical racy pattern: two parallel for iterations writing to the same shared array index — this must be a compile error.

**Warning signs:**
- Parallel for tests only cover trivial cases (array read, no shared writes)
- Test suite has no thread-safety stress tests
- Physics simulation produces nondeterministic output across runs (first sign of a race)

**Phase to address:** Phase 3d (Concurrency Checks). Add explicit test cases for method-call mutations before shipping.

---

### Pitfall 7: Comptime Interpreter Diverges / No Step Limit

**What goes wrong:**
`comptime fibonacci(50)` runs the Iron interpreter at compile time. Without a step limit, recursive or looping comptime calls that don't terminate hang the compiler indefinitely. The user cannot interrupt without killing the process. Worse: a `comptime` call in a large codebase hangs CI silently until a timeout kills the job with no useful error.

**Why it happens:**
The Phase 8 spec mentions "optionally: add a step limit" — the optionality is the trap. Implementing comptime without a step limit first is common because the happy-path tests (small inputs) always terminate. The limit feels unnecessary until the first user writes `comptime fib(1000)`.

**How to avoid:**
Implement the step limit from the first comptime commit. 1,000,000 interpreter steps is a reasonable default (matches Zig's 1,000 backwards branches, scaled up for a tree-walking interpreter). Emit a clear compile error with the partial call stack when the limit is hit. Make the limit configurable via a compiler flag (`--comptime-limit N`) but enforce a hard maximum. Test: `comptime fibonacci(40)` must either complete or fail with a clear error, never hang.

**Warning signs:**
- Comptime test suite has no test for a function that exceeds the step limit
- `iron build` on a file with `comptime` hangs in the terminal
- No timeout in the test harness for comptime tests

**Phase to address:** Phase 8 (Compile-Time Evaluation). The step limit is a day-one requirement, not an afterthought.

---

### Pitfall 8: Inheritance Layout Breaks Polymorphic Casting

**What goes wrong:**
The implementation plan compiles `Player extends Entity` by flattening fields: `typedef struct { Vec2 pos; int64_t hp; Iron_String name; float speed; } Player;`. This matches the layout of `Entity` for the first two fields, enabling `(Entity*)&player` to work. But adding fields with different alignment between parent and child, or changing the parent struct, silently breaks all child casts. The `is` runtime type check generates incorrect results when type tags are not embedded in structs.

**Why it happens:**
The field-flattening approach documented in Phase 4e only works if parent fields always come first in the child struct, in exactly the same order, with no padding differences. Any parent field reordering (during refactoring) silently corrupts child casts across the codebase. Runtime `is` checking requires a type tag embedded at a known offset — if this isn't designed in from the start, adding it later requires rewriting all struct layouts.

**How to avoid:**
Embed the parent struct as the first field (not flatten), ensuring layout compatibility by C rules: `typedef struct { Entity _base; Iron_String name; float speed; } Player;`. This guarantees `(Entity*)&player == &player._base`. Embed a type tag (integer enum) as the first field of the root base type (every type that participates in inheritance or `is` checks). Design the type tag system before writing any inheritance codegen — retrofitting requires regenerating all object layouts.

**Warning signs:**
- `is` check returns wrong results for objects two levels deep in the inheritance chain
- Adding a field to a parent object silently breaks tests for child objects
- Structs with alignment-sensitive fields (double, 64-bit integers) behave differently on 32-bit vs 64-bit targets

**Phase to address:** Phase 4e (Inheritance) — lock in the layout strategy and type tag design before any inheritance codegen is written.

---

### Pitfall 9: Lambda Closure Capture Generates Dangling Pointers

**What goes wrong:**
Iron lambdas capture outer variables by reference (per spec: "all outer variables are captured by reference implicitly"). The generated C must create a struct holding pointers to the captured variables. If the captured variable lives on the stack and the lambda outlives that stack frame (stored in an object field, passed to a spawn block), the closure contains dangling pointers. Accessing a captured variable through the lambda after the declaring scope has exited is undefined behavior in the generated C.

**Why it happens:**
Reference capture is the natural default for ergonomics (avoids copies). The spec defines it as implicit reference capture. But C function pointers cannot carry state — closures require explicit struct-based "environment" objects. When those environments contain pointers to stack-allocated locals, and the closure escapes, the pointers dangle. Escape analysis for closures is more complex than for simple heap values.

**How to avoid:**
Generate closures as a two-part struct: a function pointer and an environment struct (heap-allocated). The environment captures values (copies of primitives, references for objects). Apply escape analysis to closure captures: if the lambda escapes its declaring scope (stored in an `object` field, passed to `spawn`), ensure all captured stack variables are promoted to heap or copied into the environment. Add a semantic analysis check: assigning a lambda to an object field or passing it to `spawn` while it captures stack-frame locals is either a compile error or triggers auto-promotion. Test: lambda captured into an object field that outlives the declaring function — accessing the captured variable must not crash.

**Warning signs:**
- Lambda tests only exercise lambdas called within the same scope they are created
- Lambda stored in a `Button.on_click` field and triggered later causes segfault
- ASAN reports use-after-stack-frame in any lambda test

**Phase to address:** Phase 2 (Parser: LambdaExpr node), Phase 3 (Semantic Analysis: escape analysis extension to closures), Phase 4 (Codegen: closure struct generation).

---

### Pitfall 10: Type Inference Cascades Into Incorrect Error Messages

**What goes wrong:**
Iron infers types for `val`/`var` with no explicit annotation. A type error in an inferred-type chain produces a diagnostic pointing to a variable declaration line, not the actual mismatch site. The user sees "type mismatch at line 3" for `val x = foo()` when the real problem is that `foo()` returns the wrong type because of a bug 20 lines away. Error messages attribute blame to symptoms, not causes.

**Why it happens:**
Simple type inference works by propagating types forward. When propagation reaches a conflict, the conflict is reported at the conflict site, which may be far from the user's mistake. Without bidirectional propagation or constraint-based inference with error attribution, the compiler blames the last place types collide.

**How to avoid:**
Implement bidirectional type checking (check mode and infer mode): expressions with an expected type from context are checked against it; expressions without context infer upward. When a mismatch is found, report the full chain: "expected `Int` (from variable declared at line 3), got `Float` (from return value of `foo` at line 23)." Store "type origin" metadata on every inferred type node during semantic analysis. Test error message quality explicitly — write tests that verify not just that an error is reported, but that the message points to the right location.

**Warning signs:**
- Error message tests only check that an error exists, not its location or text
- Type errors in inferred-type chains produce cryptic messages referencing temporary expressions
- Users report that error messages are "not helpful" in early feedback

**Phase to address:** Phase 3b (Type Checking). Error message quality is a first-class requirement, not polish.

---

### Pitfall 11: Windows Pthread Compatibility Breaks Threading Phase

**What goes wrong:**
The runtime uses pthreads (`pthread_mutex_t`, `pthread_cond_wait`, `pthread_setaffinity_np`). On macOS and Linux this compiles cleanly. On Windows, pthreads are not available natively — MinGW provides a compatibility layer but `pthread_setaffinity_np` does not exist on Windows at all. The pool pinning feature (`physics.pin(2, 3)`) compiles on Linux, silently does nothing on macOS (uses `thread_policy_set` instead), and fails to compile on Windows. The "all 3 platforms from day one" constraint is broken.

**Why it happens:**
pthread abstraction leaks platform differences. `setaffinity` is a Linux-specific API. macOS uses `pthread_mach_thread_np` + `thread_policy_set`. Windows has no POSIX thread affinity API at all in the standard threading model. Developers who develop on macOS/Linux first hit this only when attempting a Windows build.

**How to avoid:**
Abstract platform threading behind an `iron_thread_platform.h` abstraction layer from the start of Phase 5. Provide no-op stubs for `pool.pin()` on platforms that don't support affinity rather than failing to compile. Use C11 `<threads.h>` where available (MSVC 17.8+ supports it) with a `tinycthread` fallback for older toolchains. Build and test on all three platforms in CI from the first threading commit. Do not use `pthread_*` directly in runtime code — use the abstraction layer only.

**Warning signs:**
- Threading code has `#include <pthread.h>` without a platform guard
- Phase 5 tests only run on macOS and Linux
- `pool.pin()` is not tested on Windows before Phase 6 begins

**Phase to address:** Phase 5 (Runtime Library). Establish the platform abstraction layer before writing any threading code.

---

### Pitfall 12: String Codegen Confuses Byte Length and Codepoint Length

**What goes wrong:**
Iron strings are "always unicode" and `len(s)` returns codepoint count. The generated C `Iron_String` struct must store both byte length and codepoint count. Generated code that uses `strlen()` or assumes `byte_length == codepoint_length` produces incorrect results for any non-ASCII string. Indexing (`s[i]`) requires O(n) scan for multi-byte codepoints unless the implementation uses UTF-32 internally, which doubles memory usage for ASCII-heavy game strings.

**Why it happens:**
The natural C string primitive is a null-terminated `char*` where `strlen` gives bytes. Unicode-correct implementations require deliberate design: either store UTF-8 with a precomputed codepoint count, or store UTF-32 for O(1) indexing. Both have tradeoffs. Developers who prototype with ASCII-only test inputs never see the failure.

**How to avoid:**
Define the `Iron_String` representation precisely before writing any string runtime code: UTF-8 encoded data (saves memory, compatible with raylib's char-pointer APIs), with a stored `codepoint_count` field separate from `byte_length`. Index operations that traverse by codepoint do so via a helper that advances through the UTF-8 sequence, not byte arithmetic. Test with Japanese, emoji, and mixed-script strings from the first string test commit. Document that `s[i]` is O(n) in the language spec — this is expected and correct for a UTF-8 string.

**Warning signs:**
- String test suite contains only ASCII strings
- `len("hello")` returns 5 but `len("日本語")` returns 9 (bytes, not codepoints)
- Indexing produces garbled output for non-ASCII input

**Phase to address:** Phase 5 (Runtime Library: String component). The `Iron_String` struct layout must be finalized before any string operations are implemented.

---

### Pitfall 13: Parser Error Recovery Produces Cascading False Errors

**What goes wrong:**
Iron requires reporting multiple errors per file. Without proper error recovery, a single missing brace causes the parser to lose synchronization and report hundreds of spurious follow-on errors. Users fix the first error, rerun the compiler, and see a completely different set of errors because the parser was reading tokens in a shifted state. Error-fixing becomes a guessing game.

**Why it happens:**
Panic-mode recovery (skip until a synchronization token) is the simplest strategy but chooses the wrong synchronization points. If the parser panics to `;` but Iron uses newlines as implicit terminators, or panics to `}` but misses that the `}` was the missing token, subsequent parses start in unexpected positions. First-pass implementations typically only handle the happy path and add error recovery as an afterthought.

**How to avoid:**
Define synchronization tokens explicitly for Iron's grammar: `func`, `object`, `interface`, `enum`, `import` keywords are reliable top-level recovery points. Inside a function body, a newline after an error expression plus the next statement-starting keyword is a recovery point. Represent errors as `ErrorNode` in the AST (not as "stop parsing") so the parser continues with degraded but operational state. Limit follow-on errors: after an error node, suppress cascade errors within the same statement. Test: a file with 3 independent syntax errors must report exactly 3 errors (not 3 + N cascades).

**Warning signs:**
- Parser error tests produce different error counts depending on the order of errors in the file
- A single missing closing brace produces 50 error lines
- Error recovery is not implemented until Phase 2 is "done" (add it during Phase 2)

**Phase to address:** Phase 2 (Parser). The implementation plan already specifies "error recovery: report multiple errors per file" — implement it during Phase 2, not after.

---

### Pitfall 14: Comptime Read-File Embeds Paths Relative to Wrong Directory

**What goes wrong:**
`comptime read_file("shaders/main.glsl")` embeds file contents at compile time. The path resolution depends on the current working directory when `iron build` is run, not the location of the `.iron` source file. Building from a different directory (e.g., CI runs `iron build src/main.iron` from the repo root) silently fails to find the shader file. The embed produces an empty or error-containing string that corrupts the shader at runtime.

**Why it happens:**
Phase 8's comptime design doesn't specify path resolution semantics. Developers naturally use `fopen(path, "r")` in the interpreter with the process's `cwd`, which is environment-dependent. The correct behavior (path relative to the source file) requires threading source file location through the comptime interpreter.

**How to avoid:**
Define path resolution for `comptime read_file` as: path relative to the source file containing the `comptime` call, not the cwd. Pass the source file's directory to the comptime interpreter explicitly. Fail with a compile error (not a runtime error) if the file does not exist at compile time, showing the resolved absolute path in the error message. Test: build the same project from two different working directories — the embedded content must be identical.

**Warning signs:**
- `comptime read_file` tests only run from the project root
- Shader/asset embed test passes locally but fails in CI
- The interpreter uses `fopen(path)` without resolving against the source file's directory

**Phase to address:** Phase 8 (Compile-Time Evaluation). Specify path resolution semantics in the Phase 8 design before implementation.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Emit user symbols without prefix | Simpler codegen initially | First identifier collision causes cryptic C error; hard to retrofit a prefix scheme | Never — establish prefix convention in Phase 4 commit 1 |
| Topological sort deferred to "later" | Ship codegen tests faster | Multi-file projects fail to compile; retrofitting sort into working codegen is risky | Never — implement before first multi-file test |
| Defer implemented as tail-only emission | Works for 90% of cases immediately | Real resource leaks under early-return paths; users discover in production | Never — add exit-point traversal from the start |
| Escape analysis that marks all function-call args as escaped | Simple, safe, no false negatives | Excessive leak warnings; users train themselves to ignore them | Acceptable as v1 if documented; must be replaced before stable release |
| Skip step limit on comptime | Ships Phase 8 faster | First user with deep recursion hangs the compiler; untestable | Never — implement step limit before first comptime test |
| pthread directly in runtime (no abstraction) | Faster Phase 5 prototype | Windows build breaks; affinity APIs platform-diverge | Never — abstraction layer before any threading code |
| ASCII-only string tests | Tests pass in minutes | Unicode bugs discovered by users, not tests | Never — add non-ASCII test strings in Phase 5 from day one |
| Parser reports only first error | Simpler parser implementation | Developers fix one error at a time; bad developer experience | Acceptable in Phase 1 (lexer only); must be fixed by end of Phase 2 |

---

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Raylib / clang invocation | Hardcode `gcc` in build pipeline | Detect `clang` first, fall back to `gcc`; store detected compiler in build config; test both |
| Raylib C API | Pass Iron object structs directly to Raylib functions expecting specific C types | Generate explicit C-struct adapters; never rely on Iron struct layout matching Raylib layout by accident |
| `pthread_setaffinity_np` | Use directly in `pool.pin()` | Wrap behind `iron_thread_set_affinity()` with platform-specific implementations; no-op on unsupported platforms |
| `-lpthread` linker flag | Assume available on all platforms | Detect and add conditionally; on macOS pthreads are included in `libSystem`, extra `-lpthread` is harmless but verify |
| `clang` vs `gcc` `-Wall -Werror` | Generate code clean under one, broken under the other | CI must compile generated code with both; they have different warning sets |
| Valgrind memory check | Run only on Linux | Valgrind is Linux/macOS; use Dr. Memory or similar on Windows; ASAN is cross-platform and preferred |

---

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Monomorphization code explosion | Binary size grows 10x when generics used heavily; compile time degrades | Limit generic instantiation depth; share implementation via function pointers for large generic structs where performance allows | When a game uses `List[T]`, `Map[K,V]`, `Set[T]` for 20+ distinct types |
| O(n) codepoint indexing in tight loops | `s[i]` inside a `for` loop over string length is O(n^2) | Document `s[i]` complexity; provide a codepoint iterator that amortizes traversal | Any string processing loop over > 100 characters |
| Comptime embeds large binary files | `comptime read_file("assets/hero.png")` embeds entire binary; generated C string literal exceeds compiler limits | Document that `comptime read_file` is for text/shader assets; add a file size limit (e.g., 1MB) with a clear error | First user who tries to embed a large texture |
| Parallel for on small collections | Thread pool submission overhead exceeds work; slower than sequential | Emit a minimum-work threshold: parallel for on < N items (e.g., 64) falls back to sequential automatically | Any game loop parallelizing tiny particle lists |
| Thread pool per-frame allocation | `pool_submit` allocates a work item per iteration; GC pressure in tight game loops | Pre-allocate work item pool in the thread pool implementation; reuse items | Game with 10,000 particles using `parallel for` at 60fps |

---

## "Looks Done But Isn't" Checklist

- [ ] **Defer:** Verify deferred code runs on ALL control flow paths (early return, break, continue) — not just at end of function. Use a test with a deferred counter increment; any missed path produces a count < expected.
- [ ] **Escape Analysis:** Verify auto-free actually calls `free()` in generated C. Use valgrind/ASAN — the absence of a warning from the Iron compiler is not sufficient.
- [ ] **String Unicode:** Verify `len("日本語") == 3` and `"日本語"[0] == "日"`. ASCII-only tests do not validate unicode correctness.
- [ ] **Inheritance Polymorphism:** Verify `is` returns correct results at 3+ levels of inheritance depth (`C extends B extends A`, `c is A` must be true). Single-level tests miss multi-level failures.
- [ ] **Parallel For Safety:** Verify that a parallel for body mutating a captured outer variable is rejected at compile time, not just documented as undefined behavior.
- [ ] **Comptime Step Limit:** Verify `comptime fibonacci(50)` either terminates within the limit or produces a clear compile error — never hangs the compiler indefinitely.
- [ ] **Cross-Platform Build:** Verify `iron build` produces a binary on all three platforms (macOS, Linux, Windows) from the Phase 5 threading code onward — not just the first time Phase 7 is done.
- [ ] **Name Collision:** Verify an Iron program that uses identifiers `signal`, `error`, `list`, `string`, `file` compiles without C-level conflicts.
- [ ] **Forward Declarations:** Verify a program with two mutually-referencing objects (each holding a pointer to the other) compiles correctly.
- [ ] **Comptime read_file Path:** Verify `comptime read_file` resolves paths relative to the source file, not the working directory. Build from two different directories and compare output.

---

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Naming collision scheme retrofitted | HIGH | Must rename all generated symbols; breaks all existing compiled code and tests; requires coordinated find-and-replace across codegen and runtime |
| Topological sort added after multi-file codegen shipped | MEDIUM | Add sort pass before emission; requires audit of all struct-dependency logic already written |
| Defer exit-point fix added after Phase 4 ships | MEDIUM | Must audit every generated function that contains a defer + early return; add regression tests for each case found |
| Escape analysis precision improved | LOW | Replacing the escape analysis algorithm does not change the language semantics; tests catch regressions |
| Step limit added to comptime after first hang report | LOW | Add step counter to interpreter; wire to compiler error path; ship as patch |
| Platform threading abstraction added after direct pthread use | MEDIUM | Wrap all pthread calls behind abstraction layer; Windows build tests reveal remaining gaps |
| Unicode string bug fixed after ASCII-only tests passed | LOW–MEDIUM | Fix the affected string operations; add unicode test suite; severity depends on how deeply string indexing is used |
| Type inference error attribution improved | LOW | Improve error message metadata without changing type inference algorithm; no language semantics change |

---

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Generated C naming collisions | Phase 4 (Codegen bootstrap) | Compile an Iron program using identifiers `signal`, `error`, `file`; no C-level errors |
| Missing forward declarations | Phase 4 (Codegen, struct emission order) | Two mutually-referencing objects compile under `-Wall -Werror` |
| Defer missing early returns | Phase 4d (Defer Implementation) | Function with defer + 3 early returns; ASAN shows no leaks on all paths |
| Escape analysis over-approximation | Phase 3c (Escape Analysis) | Valgrind on auto-free test shows zero leaks; no spurious warnings on common patterns |
| RC cycles leak memory | Phase 5 (Runtime) + Phase 3 (docs) | Scene load/unload loop under valgrind shows stable memory; compiler warns on structural rc cycles |
| Parallel for mutation not caught | Phase 3d (Concurrency Checks) | Racy parallel for pattern rejected at compile time with useful error |
| Comptime divergence | Phase 8 (Comptime Evaluation) | `comptime fibonacci(50)` either terminates or errors within 5 seconds; never hangs |
| Inheritance layout / is check | Phase 4e (Inheritance Codegen) | `is` correct at 3 inheritance levels; parent field reorder doesn't silently break child tests |
| Lambda closure dangling pointer | Phase 3 (Semantic Analysis) + Phase 4 | Lambda stored in object field and triggered after declaring scope exits — ASAN clean |
| Type inference error attribution | Phase 3b (Type Checking) | Error message tests verify file + line + helpful text, not just error existence |
| Windows pthread compatibility | Phase 5 (Runtime Library) | CI runs on Windows from Phase 5 onward; `pool.pin()` compiles and is a no-op on Windows |
| String unicode length/index | Phase 5 (Runtime Library: String) | `len("日本語") == 3`; `"日本語"[1] == "本"` |
| Parser error cascade | Phase 2 (Parser) | File with 3 independent syntax errors produces exactly 3 error messages |
| Comptime read_file path resolution | Phase 8 (Comptime Evaluation) | Build from two directories; embedded content is identical |

---

## Sources

- Zig comptime design analysis: https://matklad.github.io/2025/04/19/things-zig-comptime-wont-do.html
- Comptime downsides (C3 language blog): https://c3.handmade.network/blog/p/8590-the_downsides_of_compile_time_evaluation
- Defer implementation edge cases: https://dev.to/lerno/implementing-defer-3l76
- Resilient recursive descent parsing: https://thunderseethe.dev/posts/parser-base/
- SEI CERT C reserved identifiers: https://wiki.sei.cmu.edu/confluence/display/c/DCL37-C.+Do+not+declare+or+define+a+reserved+identifier
- Cyclic reference counting (ISMM 2024): https://dl.acm.org/doi/10.1145/3652024.3665507
- Struct layout and inheritance in C: https://www.embedded.com/programming-embedded-systems-inheritance-in-c-and-c/
- Flow-sensitive nullable type narrowing: https://github.com/microsoft/TypeScript/issues/37802
- Monomorphization code bloat: https://lobste.rs/s/aar0zx/dark_side_inlining_monomorphization
- C11 threads on Windows (MSVC 17.8): https://devblogs.microsoft.com/cppblog/c11-threads-in-visual-studio-2022-version-17-8-preview-2/
- TinyCThread cross-platform threading: https://tinycthread.github.io/
- Forward declarations and topological sort: https://thelinuxcode.com/forward-declarations-in-c-and-c-how-i-use-them-to-break-cycles-shrink-headers-and-keep-builds-sane-2026/
- Arena allocators for compiler development: https://www.rfleury.com/p/untangling-lifetimes-the-arena-allocator
- Parser error recovery: https://tratt.net/laurie/blog/2020/automatic_syntax_error_recovery.html

---
*Pitfalls research for: Iron compiler (compiled language / transpile-to-C)*
*Researched: 2026-03-25*
