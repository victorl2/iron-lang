# Phase 4: Comptime, Game Dev, and Cross-Platform - Context

**Gathered:** 2025-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

Comptime evaluation works, raylib programs build and run, and the full toolchain produces binaries on macOS, Linux, and Windows. This is the final phase — it delivers: compile-time evaluation with caching, raylib bindings via a hand-written raylib.iron wrapper, the `extern func` FFI mechanism, `draw {}` block lowering, Windows parity with clang-cl, and full CI on all 3 platforms.

</domain>

<decisions>
## Implementation Decisions

### Comptime Evaluator
- 1 million step limit: each loop iteration and function call counts as one step; violation produces a compile error with the step count
- User functions supported: `comptime build_sin_table()` evaluates user-defined Iron functions at compile time
- All result types: primitives (Int, Float, Bool, String), arrays ([Float; 360]), and structs (Iron_Vec2) can be comptime results — serialized as C literals
- `comptime read_file()` resolves paths relative to the source file, not the project root
- Compile error with call trace: "error in comptime: division by zero at sin_table.iron:5, called from main.iron:12"
- Content-hash cache: comptime results cached in `.iron-build/comptime/` keyed by function source hash; `--force-comptime` CLI flag overrides cache
- Full string support in comptime: concat, substring, interpolation all work
- Same types as runtime: comptime uses the same Iron type system
- Loops supported with step counting (each iteration counts toward 1M limit)
- Recursion supported with step counting (each call frame counts toward limit)
- Tree-walking AST interpreter: walks the annotated AST directly, no separate bytecode IR
- Strict error on heap: any `heap` keyword in comptime context is an immediate compile error
- Comptime restrictions enforced: no heap, no rc, no runtime I/O (except read_file)

### Raylib Bindings
- Hand-written subset: manually curated bindings for the most-used raylib functions — not auto-generated
- Bundled with Iron: raylib source (single-file header raylib.c) compiled inline alongside generated C — zero system dependency
- `draw {}` lowers to `BeginDrawing(); { body } EndDrawing();` — can appear anywhere, not restricted to game loop
- raylib.iron wrapper file ships with the compiler: parsed like any import, contains extern declarations
- Color constants (RED, BLUE, DARKGRAY, WHITE) available only via `import raylib`
- Vec2/Color as Iron objects: `object Vec2 { var x: Float; var y: Float }` that match C struct layout
- Key codes as Iron enum with dot syntax: `is_key_down(.RIGHT)` per README
- Function categories included: window+input, drawing 2D, textures, audio basics
- Auto-convert Iron_String to C string: when passing Iron_String to extern func expecting `const char*`, compiler inserts conversion call automatically

### extern Keyword (FFI)
- `extern func` keyword: `extern func init_window(w: Int, h: Int, title: String)` tells codegen to emit a direct C call
- Functions only: no `extern val` in v1 — globals accessed via extern getters if needed
- iron.toml handles linking: `[dependencies] raylib = true` — extern declarations just declare symbols
- Default C calling convention only
- 64-bit only: Iron only targets/ships 64-bit binaries, no 32-bit support
- Iron objects that match C layout for FFI types — no `extern object` keyword, regular `object` with matching field layout

### Windows Parity
- clang-cl on Windows: use MSVC-compatible clang (clang-cl) for Windows compilation
- C11 threads on Windows: MSVC supports `<threads.h>` — use C11 threads instead of pthreads for Windows runtime
- Platform-aware binary output: `hello.iron` → `hello` on Unix, `hello.exe` on Windows
- Normalize paths to / internally: forward slashes everywhere in Iron, convert at system boundaries (like Go)
- Add windows-latest to CI matrix: same GitHub Actions workflow, extended matrix
- RT-08 Windows completion: macOS+Linux was Phase 3, Windows added here

### Build Pipeline
- Compile raylib source inline: bundle raylib.c and compile alongside generated C when iron.toml has `raylib = true`
- Platform-aware .exe extension
- `--force-comptime` flag added to `iron build` and `iron run`

### Carried Forward
- Iron_ prefix on everything
- Clang-only compiler target (clang on macOS/Linux, clang-cl on Windows)
- Generated C compiles with clang -std=c11 -Wall -Werror
- E0001-style error codes
- iron.toml with `[dependencies] raylib = true`
- Existing compiler pipeline: lex → parse → analyze → codegen → clang

### Claude's Discretion
- Exact AST interpreter implementation details
- Comptime value representation (tagged union for interpreter values)
- Raylib.iron wrapper file exact function signatures
- How many raylib functions to include in each category
- Comptime cache file format (JSON, binary, etc.)
- C11 threads vs pthreads abstraction layer implementation
- Windows-specific path normalization details
- Exact clang-cl flags for Windows builds

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Language Specification
- `docs/language_definition.md` — Comptime restrictions, `comptime` keyword usage, `draw {}` block syntax, enum syntax for key codes
- `docs/implementation_plan.md` — Phase 8 (comptime approach, restrictions, mini interpreter), Phase 4 (codegen type mapping for C output)

### Research
- `.planning/research/PITFALLS.md` — Comptime step limit mandatory (Zig experience), rc cycles in entity graphs
- `.planning/research/ARCHITECTURE.md` — Comptime placement between semantic analysis and codegen
- `.planning/research/STACK.md` — Raylib 5.5 recommendation

### Phase 1-3 Code (MUST READ)
- `src/codegen/codegen.c` — Main wrapper (iron_runtime_init/shutdown around Iron_main), includes buffer, single .c output
- `src/codegen/gen_exprs.c` — Expression emission patterns (how extern calls should emit)
- `src/codegen/gen_stmts.c` — Statement emission (where draw {} block lowering will go)
- `src/codegen/gen_types.c` — Type mapping (Iron → C), monomorphization — extern types must fit this
- `src/analyzer/resolve.c` — Name resolution (needs to handle extern declarations and import raylib)
- `src/analyzer/typecheck.c` — Type checking (needs comptime restriction enforcement)
- `src/cli/build.c` — Build pipeline (needs raylib compilation, comptime cache, --force-comptime)
- `src/cli/toml.c` — iron.toml parser (already parses `[dependencies] raylib = true`)
- `src/runtime/iron_runtime.h` — Runtime header (threading abstraction needs C11 threads path for Windows)
- `src/runtime/iron_threads.c` — pthreads implementation (needs C11 threads alternative for Windows)
- `src/parser/ast.h` — AST node types (IRON_NODE_COMPTIME already exists, needs interpreter)

### Project Context
- `.planning/PROJECT.md` — Core value, constraints (Raylib target, cross-platform)
- `.planning/REQUIREMENTS.md` — CT-01..04, GAME-01..04 mapped to this phase
- `.planning/phases/03-runtime-stdlib-and-cli/03-CONTEXT.md` — Phase 3 decisions (pthreads, CLI behavior, iron.toml)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `src/parser/ast.h`: IRON_NODE_COMPTIME already exists in the AST — interpreter needs to handle this node
- `src/codegen/gen_exprs.c`: Expression emission patterns — extern func calls follow same pattern but skip Iron_ prefix
- `src/cli/toml.c`: Already parses `[dependencies] raylib = true` — build.c needs to act on this
- `src/cli/build.c`: Full build pipeline — needs extension for raylib linking and comptime cache
- `src/runtime/iron_threads.c`: pthreads implementation — Windows needs alternative path via C11 threads or abstraction
- `src/diagnostics/diagnostics.h`: E-code system — comptime errors use same system

### Established Patterns
- Iron_ prefix on all types and functions
- Arena allocation for compiler data structures
- stb_ds hash maps for registries (monomorphization, intern table, scope symbols)
- Unity test framework with CMake FetchContent
- posix_spawn for subprocess invocation (clang, binary execution)
- isatty for ANSI color gating

### Integration Points
- Comptime evaluator runs AFTER semantic analysis, BEFORE codegen — replaces IRON_NODE_COMPTIME with literal values in the AST
- Raylib bindings need: lexer (extern keyword), parser (extern func declarations), resolver (import raylib), codegen (extern call emission, draw block lowering)
- Windows parity needs: runtime threading abstraction, build.c clang-cl invocation, CI matrix extension
- `iron build` needs: comptime cache check, raylib source compilation, --force-comptime flag

</code_context>

<specifics>
## Specific Ideas

- The README game example should be compilable and runnable by the end of this phase — it uses `import raylib`, `draw {}`, objects, methods, and `is_key_down(.RIGHT)`
- Comptime `build_sin_table()` from the README should work: loops, array construction, math, and return a [Float; 360]
- `comptime read_file("shaders/main.glsl")` embeds shader source at compile time — important for game dev
- Raylib single-file header (raylib.c) approach means zero external dependencies for Iron game builds

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 04-comptime-game-dev-and-cross-platform*
*Context gathered: 2025-03-26*
