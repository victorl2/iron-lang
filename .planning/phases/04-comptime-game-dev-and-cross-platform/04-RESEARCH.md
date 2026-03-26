# Phase 4: Comptime, Game Dev, and Cross-Platform - Research

**Researched:** 2026-03-25
**Domain:** Compile-time evaluation, raylib FFI, extern keyword, Windows parity
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Comptime Evaluator**
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

**Raylib Bindings**
- Hand-written subset: manually curated bindings for the most-used raylib functions — not auto-generated
- Bundled with Iron: raylib source (single-file header raylib.c) compiled inline alongside generated C — zero system dependency
- `draw {}` lowers to `BeginDrawing(); { body } EndDrawing();` — can appear anywhere, not restricted to game loop
- raylib.iron wrapper file ships with the compiler: parsed like any import, contains extern declarations
- Color constants (RED, BLUE, DARKGRAY, WHITE) available only via `import raylib`
- Vec2/Color as Iron objects: `object Vec2 { var x: Float; var y: Float }` that match C struct layout
- Key codes as Iron enum with dot syntax: `is_key_down(.RIGHT)` per README
- Function categories included: window+input, drawing 2D, textures, audio basics
- Auto-convert Iron_String to C string: when passing Iron_String to extern func expecting `const char*`, compiler inserts conversion call automatically

**extern Keyword (FFI)**
- `extern func` keyword: `extern func init_window(w: Int, h: Int, title: String)` tells codegen to emit a direct C call
- Functions only: no `extern val` in v1 — globals accessed via extern getters if needed
- iron.toml handles linking: `[dependencies] raylib = true` — extern declarations just declare symbols
- Default C calling convention only
- 64-bit only: Iron only targets/ships 64-bit binaries, no 32-bit support
- Iron objects that match C layout for FFI types — no `extern object` keyword, regular `object` with matching field layout

**Windows Parity**
- clang-cl on Windows: use MSVC-compatible clang (clang-cl) for Windows compilation
- C11 threads on Windows: MSVC supports `<threads.h>` — use C11 threads instead of pthreads for Windows runtime
- Platform-aware binary output: `hello.iron` → `hello` on Unix, `hello.exe` on Windows
- Normalize paths to / internally: forward slashes everywhere in Iron, convert at system boundaries (like Go)
- Add windows-latest to CI matrix: same GitHub Actions workflow, extended matrix
- RT-08 Windows completion: macOS+Linux was Phase 3, Windows added here

**Build Pipeline**
- Compile raylib source inline: bundle raylib.c and compile alongside generated C when iron.toml has `raylib = true`
- Platform-aware .exe extension
- `--force-comptime` flag added to `iron build` and `iron run`

**Carried Forward**
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

### Deferred Ideas (OUT OF SCOPE)
None — discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| CT-01 | `comptime` expressions evaluate at compile time and emit result as literals | Interpreter design, value serialization to C literals, AST replacement patterns |
| CT-02 | `comptime read_file()` embeds file contents as string/byte array at compile time | File I/O at compile time, path resolution relative to source file, size limits |
| CT-03 | Comptime restrictions enforced: no heap, no rc, no runtime I/O | Restriction detection in typecheck/interpreter, IRON_NODE_HEAP/RC detection |
| CT-04 | Step limit prevents infinite loops during compile-time evaluation | Step counter design, 1M limit, error emission pattern from PITFALLS.md |
| GAME-01 | Raylib bindings allow Iron programs to create windows, draw, and handle input | extern func keyword, raylib.iron wrapper, resolver changes, codegen emission |
| GAME-02 | The draw {} block syntax works for raylib begin/end drawing | New IRON_NODE_DRAW AST node or lowering at parse/codegen, BeginDrawing pattern |
| GAME-03 | Compiled binaries are standalone executables (runtime statically linked) | Already true from Phase 3; raylib.c bundled compilation in build.c |
| GAME-04 | Build produces working binaries on macOS, Linux, and Windows | clang-cl invocation, C11 threads abstraction, CI matrix, .exe extension |
| RT-08 | Runtime compiles and passes tests on macOS, Linux, and Windows | Windows threading abstraction: C11 threads instead of pthreads |
</phase_requirements>

---

## Summary

Phase 4 is the final phase. It has four distinct technical domains that can be implemented in parallel within the wave structure: (1) the comptime tree-walking AST interpreter, (2) raylib bindings via `extern func` and the draw block, (3) Windows parity via C11 threads and clang-cl, and (4) build pipeline extensions for raylib compilation and the comptime cache.

The existing compiler infrastructure is complete: all 20 tests pass, the full pipeline from lex through codegen runs, `build.c` handles the clang invocation, and `iron.toml` already parses `raylib = true`. The most complex new subsystem is the comptime interpreter — it must walk the annotated AST, carry its own value representation (tagged union), enforce restrictions via the type checker or interpreter, and serialize results as C literals that replace the `IRON_NODE_COMPTIME` node before codegen runs.

The raylib integration requires adding `extern func` to the lexer, parser, resolver, and codegen, plus writing a `raylib.iron` wrapper file that the resolver treats as a special import. The `draw {}` block is a new statement node (or handled as a parser lowering). The Windows parity work is isolated to the runtime threading abstraction and the build.c clang invocation.

**Primary recommendation:** Implement in this wave order: (1) extern func + draw block + raylib.iron wrapper, (2) comptime evaluator with step limit + restrictions, (3) comptime cache + read_file, (4) Windows C11 threads + clang-cl + CI matrix. Each wave produces testable output.

---

## Standard Stack

### Core
| Component | What It Is | Why It's Used |
|-----------|-----------|---------------|
| raylib 5.5 | Single-file game library | Target graphics backend per language spec; single-file header bundling matches zero-dependency goal |
| C11 `<threads.h>` | ISO C threading | Available on MSVC 17.8+ and clang-cl; replaces pthreads on Windows for RT-08 |
| clang-cl | MSVC-compatible clang driver | Windows target for Iron binaries; matches existing clang-only policy |

### Supporting
| Component | What It Is | When to Use |
|-----------|-----------|-------------|
| SHA-256 or FNV-1a hash | Content hash for comptime cache | Cache key for `.iron-build/comptime/` files; FNV-1a is simpler, no external dep |
| tinycthread | C11 threads portable shim | Fallback if platform lacks `<threads.h>` (older MinGW); check availability first |
| posix_spawn (macOS/Linux) / CreateProcess (Windows) | Process spawning | Already used in build.c; clang-cl invocation on Windows needs CreateProcess or cross-platform spawn |

### Raylib Version Note
Raylib 5.5 is recommended (per `.planning/research/STACK.md`). The single-file `raylib.c` distribution is available from the raylib releases page. The `raylib.h` header and `raylib.c` implementation are bundled together in some distributions; the exact bundled file is `raylib.c` which includes `raylib.h` internally.

**Installation (raylib source for bundling):**
```bash
# Download raylib.c and raylib.h from https://github.com/raysan5/raylib/releases/tag/5.5
# Place in src/vendor/raylib/ inside the iron-lang repo
# No system installation needed — compiled inline by iron build
```

---

## Architecture Patterns

### Compiler Pipeline Position for Comptime

```
.iron source → Lex → Parse → Analyze → [COMPTIME EVAL] → Codegen → clang → binary
```

The comptime evaluator runs AFTER semantic analysis (the AST has resolved types and symbols) and BEFORE codegen. It finds all `IRON_NODE_COMPTIME` nodes, evaluates them, and REPLACES them in the AST with literal nodes (`IRON_NODE_INT_LIT`, `IRON_NODE_FLOAT_LIT`, `IRON_NODE_STRING_LIT`, `IRON_NODE_ARRAY_LIT`). Codegen then emits these literals directly.

This is the correct integration point because:
- `resolved_type` is available on all nodes after analysis
- `resolved_sym` is available for function lookup
- No new AST walk infrastructure needed — just a new pass in `analyzer.c`

### Comptime Interpreter Value Representation

Use a tagged union for interpreter values (Claude's discretion):

```c
typedef enum {
    IRON_CVAL_INT,
    IRON_CVAL_FLOAT,
    IRON_CVAL_BOOL,
    IRON_CVAL_STRING,
    IRON_CVAL_ARRAY,
    IRON_CVAL_STRUCT,
    IRON_CVAL_NULL,
} Iron_ComptimeValKind;

typedef struct Iron_ComptimeVal {
    Iron_ComptimeValKind kind;
    union {
        int64_t   as_int;
        double    as_float;
        bool      as_bool;
        Iron_String as_string;
        struct {
            struct Iron_ComptimeVal **elems;
            int                       count;
        } array;
        struct {
            const char             **field_names;
            struct Iron_ComptimeVal **field_vals;
            int                       field_count;
        } object;
    };
} Iron_ComptimeVal;
```

All values live in the arena. No heap allocation inside the comptime interpreter.

### Step Counter Design

```c
typedef struct {
    Iron_Arena       *arena;
    Iron_DiagList    *diags;
    Iron_Scope       *global_scope;
    int               steps;          /* current step count */
    int               step_limit;     /* 1,000,000 by default */
    /* Call stack for error trace */
    const char      **call_stack;     /* stb_ds array of func names */
    Iron_Span        *call_spans;     /* stb_ds array of call site spans */
    int               call_depth;
    /* Source file path for read_file resolution */
    const char       *source_file_dir;
} Iron_ComptimeCtx;
```

Each function call: `if (++ctx->steps > ctx->step_limit) { emit_step_limit_error(); return NULL; }`
Each loop iteration body exit: same check.

### extern func in the Pipeline

**Lexer:** `extern` keyword already present as a keyword (needs to be added to the keyword list if not already there — check `lexer.c`). The token sequence is `KEYWORD(extern) KEYWORD(func) IDENT(name) ...`

**Parser:** New `IRON_NODE_EXTERN_FUNC_DECL` node OR reuse `Iron_FuncDecl` with an `is_extern` flag. Using a flag on `Iron_FuncDecl` is cheaper and avoids new AST node types. Add `bool is_extern` to `Iron_FuncDecl`.

**Resolver:** `extern func` declarations are registered in the global scope as `IRON_SYM_FUNC` symbols with `is_extern = true`. No body to resolve. When processing `import raylib`, the resolver loads `raylib.iron` from the compiler's standard library path and processes it as if it were a regular import.

**Codegen:** When emitting a `IRON_NODE_CALL` where the callee's resolved symbol has `is_extern = true`, skip the `Iron_` prefix mangling and emit the raw C function name. Example: `init_window(...)` → `InitWindow(...)` (raylib uses CamelCase). The mapping from Iron name to C name is in the `raylib.iron` wrapper.

**Auto String→C conversion:** When an extern func parameter expects `String` and the Iron call passes a String, codegen wraps the argument: `iron_string_cstr(&arg)` to produce `const char*`.

### draw {} Block Lowering

The `draw {}` block can be handled two ways:
1. New `IRON_NODE_DRAW` in the parser, lowered in `gen_stmts.c`
2. Parser lowers immediately to a call sequence (simpler, no new AST node)

**Recommended approach (new AST node):** Add `IRON_NODE_DRAW` to `Iron_NodeKind`. The parser recognizes `draw {` as the draw block keyword (needs lexer keyword) and creates the node. `gen_stmts.c` emits it as:

```c
BeginDrawing();
{
    /* body */
}
EndDrawing();
```

This is cleaner for the type checker (can enforce no draw blocks inside comptime) and AST pretty-printer.

**Alternative (no new node):** The parser treats `draw {` as syntactic sugar and immediately emits an `IRON_NODE_BLOCK` with a `BeginDrawing()` call prepended and `EndDrawing()` deferred. Simpler but harder to identify in the AST later.

New node approach is preferred — it costs one new `IRON_NODE_DRAW` in the enum and a struct `Iron_DrawBlock` with a single `body` field.

### raylib.iron Wrapper Structure

```iron
-- raylib.iron (ships with compiler, not user-editable)
-- Extern declarations for the Iron raylib binding

-- Window management
extern func init_window(w: Int, h: Int, title: String)    -- maps to InitWindow
extern func close_window()                                  -- maps to CloseWindow
extern func window_should_close() -> Bool                   -- maps to WindowShouldClose
extern func get_frame_time() -> Float                       -- maps to GetFrameTime

-- Drawing
extern func clear_background(color: Color)                 -- maps to ClearBackground
extern func begin_drawing()                                -- maps to BeginDrawing
extern func end_drawing()                                  -- maps to EndDrawing
extern func draw_text(text: String, x: Int, y: Int, size: Int, color: Color)
extern func draw_circle(pos: Vec2, radius: Float, color: Color)
extern func draw_texture(tex: Texture, pos: Vec2)

-- Input
extern func is_key_down(key: Key) -> Bool                  -- maps to IsKeyDown
extern func is_key_pressed(key: Key) -> Bool               -- maps to IsKeyPressed

-- Type declarations (match C struct layout)
object Vec2 { var x: Float; var y: Float }
object Color { var r: UInt8; var g: UInt8; var b: UInt8; var a: UInt8 }
object Texture { var id: UInt; var width: Int; var height: Int; ... }

-- Color constants
val RED      = Color(230, 41, 55, 255)
val BLUE     = Color(0, 121, 241, 255)
val DARKGRAY = Color(80, 80, 80, 255)
val WHITE    = Color(255, 255, 255, 255)

-- Key enum
enum Key {
    RIGHT, LEFT, UP, DOWN, SPACE, ...
}
```

The codegen for extern calls needs a name-mapping table: Iron `init_window` → C `InitWindow`. This can be a simple `extern_c_name` field on the symbol, set when the resolver processes the `extern func` declaration. The mapping is: Iron snake_case → raylib CamelCase. A helper that converts is sufficient.

### Windows: C11 Threads Abstraction

The locked decision is to use C11 `<threads.h>` on Windows. The strategy:

```c
/* iron_threads.h */
#ifdef _WIN32
  #include <threads.h>   /* C11 threads — available on MSVC 17.8+ */
  typedef thrd_t         Iron_ThreadHandle;
  typedef mtx_t          Iron_Mutex_t;
  typedef cnd_t          Iron_Cond_t;
  #define IRON_THREAD_CREATE(t, fn, arg)  thrd_create(&(t), (fn), (arg))
  /* ... */
#else
  #include <pthread.h>
  typedef pthread_t       Iron_ThreadHandle;
  typedef pthread_mutex_t Iron_Mutex_t;
  typedef pthread_cond_t  Iron_Cond_t;
  #define IRON_THREAD_CREATE(t, fn, arg)  pthread_create(&(t), NULL, (fn), (arg))
  /* ... */
#endif
```

The existing `iron_threads.c` uses `pthread_t`, `pthread_mutex_t`, `pthread_cond_t` directly. The approach is to introduce these abstraction macros/typedefs in `iron_runtime.h` and change `iron_threads.c` to use them. The logic stays identical — only the underlying primitives change.

Note: C11 thread function signature is `int fn(void*)` vs pthread's `void* fn(void*)`. The return type differs — `thrd_create` expects `int (*)(void*)`. This requires a thin wrapper or conditional compilation for the worker function.

### Comptime Cache Design

Cache key: FNV-1a hash of the function source text (the substring of the source file from function start to end). This is stable across recompiles as long as the function body doesn't change.

Cache file location: `.iron-build/comptime/<hex-hash>.json`

Cache format (Claude's discretion — JSON recommended for debuggability):
```json
{
  "func_name": "build_sin_table",
  "result_type": "array_float_360",
  "c_literal": "{ 0.0, 0.01745..., ... }"
}
```

On cache hit: read the `c_literal` string, wrap it in the appropriate AST literal node and splice it in.

`--force-comptime` flag: add `bool force_comptime` to `IronBuildOpts` in `build.h`. When set, skip the cache read (but still write the cache after evaluation).

### Recommended Project Structure for New Files

```
src/
├── comptime/
│   ├── comptime.h          -- Iron_ComptimeCtx, iron_comptime_eval()
│   └── comptime.c          -- tree-walking interpreter
├── runtime/
│   ├── iron_runtime.h      -- add threading abstraction macros
│   └── iron_threads.c      -- use abstraction macros
├── vendor/
│   └── raylib/
│       ├── raylib.h        -- raylib 5.5 header
│       └── raylib.c        -- raylib 5.5 single-file implementation
└── stdlib/
    └── raylib.iron         -- hand-written extern declarations
```

New AST node in `ast.h`: `IRON_NODE_DRAW` (and `IRON_NODE_EXTERN_FUNC_DECL` if a new node is chosen over a flag on `Iron_FuncDecl`).

New flag on existing node: `bool is_extern` on `Iron_FuncDecl`.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Raylib API bindings | Auto-generated from raylib.h | Hand-written raylib.iron subset | Decided; auto-gen has fragile edge cases; hand-written gives precise Iron semantics |
| String→char* conversion | Custom converter | `iron_string_cstr()` already in runtime | Function already exists in `iron_string.c`; just call it in codegen |
| Threading on Windows | Win32 threads directly | C11 `<threads.h>` / tinycthread | C11 threads are ISO standard, available on MSVC 17.8+; consistent API |
| Cache key | AST serialization | FNV-1a hash of source text substring | Source text is deterministic and available; AST serialization is complex |
| Content-addressable storage | Custom DB | Filesystem `.iron-build/comptime/<hash>` files | Simple, debuggable, no dependencies |

**Key insight:** The existing `iron_string_cstr()` already returns `const char*` from an `Iron_String`. The entire String→C conversion in extern calls is a one-line codegen insertion.

---

## Common Pitfalls

### Pitfall 1: Comptime Step Limit Not Implemented from Day One
**What goes wrong:** Implementing the interpreter without the step counter first, planning to add it "later". First user with a recursive comptime function hangs the compiler indefinitely.
**Why it happens:** Happy-path tests always terminate. The limit feels unnecessary until it isn't.
**How to avoid:** The `Iron_ComptimeCtx` struct MUST include `steps` and `step_limit` from the first commit. Every function call and loop iteration increments `steps`. The limit check is one `if` statement.
**Warning signs:** Comptime tests have no test that exceeds the step limit.

### Pitfall 2: Comptime read_file Resolves Against cwd, Not Source File
**What goes wrong:** `comptime read_file("shaders/main.glsl")` resolves against the cwd of `iron build`, not the `.iron` file's directory. Builds from different directories produce different results or fail.
**Why it happens:** `fopen(path)` uses cwd by default.
**How to avoid:** Pass `source_file_dir` in `Iron_ComptimeCtx`. In the `read_file` builtin handler, prepend `source_file_dir` to the path before opening.
**Warning signs:** `comptime read_file` tests only run from the project root.

### Pitfall 3: extern func Name Mangling Conflict
**What goes wrong:** Codegen mangles `init_window` to `Iron_init_window` which doesn't exist in raylib (it's `InitWindow`). The C compiler fails with "undefined symbol".
**Why it happens:** The regular call emission path always mangles names with `Iron_` prefix.
**How to avoid:** Check `resolved_sym->is_extern` in `emit_expr` for `IRON_NODE_CALL`. When true, use the `extern_c_name` field from the symbol (set at resolver time) instead of mangling. The `extern_c_name` for `init_window` is `InitWindow`.
**Warning signs:** Extern call tests compile without C errors but only if the extern name happens to be the same as Iron's name.

### Pitfall 4: String→C Conversion Not Inserted for extern Calls
**What goes wrong:** `init_window(800, 600, "My Game")` generates `InitWindow((int64_t)800, (int64_t)600, iron_string_from_literal("My Game", 7))` — but `InitWindow` expects `const char*`, not `Iron_String`. Clang emits a type mismatch error.
**Why it happens:** The regular argument emission path emits `Iron_String` for string literals. Extern calls need `const char*` for string params.
**How to avoid:** In `emit_expr` for `IRON_NODE_CALL` with an extern callee, check each argument type. If the argument's `resolved_type` is `IRON_TYPE_STRING`, wrap the emission: `iron_string_cstr(&<arg>)`.
**Warning signs:** First raylib test with a string argument fails to compile.

### Pitfall 5: draw {} Block Conflicts with IRON_NODE_BLOCK
**What goes wrong:** The parser treats `draw {` as a block starting with an identifier named `draw`, producing confusing parse errors or incorrect AST.
**Why it happens:** `draw` is not yet a keyword in the lexer/parser.
**How to avoid:** Add `draw` to the keyword list in the lexer. The keyword must be added alongside `comptime`, `extern`, etc. Without this, the parser cannot distinguish `draw {` from a call to `draw()`.
**Warning signs:** Parsing `draw { clear(DARKGRAY) }` produces an AST with `draw` as a function call with a block argument instead of a draw statement.

### Pitfall 6: Vec2/Color Object Layout Doesn't Match raylib C Structs
**What goes wrong:** Iron's `object Color { var r: UInt8; var g: UInt8; var b: UInt8; var a: UInt8 }` generates a C struct with padding or different field sizes than raylib's `Color { unsigned char r, g, b, a; }`. Passing Iron Color to raylib drawing functions produces wrong colors.
**Why it happens:** Iron uses `uint8_t` for `UInt8`, but if the struct has unexpected padding the layout differs.
**How to avoid:** Add a `static_assert(sizeof(Iron_Color) == sizeof(Color), ...)` in the generated C or in a test. Map Iron `UInt8` → `unsigned char` (which is the same as `uint8_t` on all targets). Vec2 maps `Float` → `float` (raylib uses `float`, not `double`) — this is the key one to get right. Iron `Float` maps to `double` normally, but for raylib compatibility Vec2 fields must be `float`. The wrapper must declare `var x: Float32; var y: Float32`.

### Pitfall 7: Windows CI Fails Because posix_spawn Not Available
**What goes wrong:** `build.c` uses `posix_spawn` and `waitpid` which are POSIX-only. Compiling the Iron compiler itself on Windows fails.
**Why it happens:** The entire compiler build system uses POSIX subprocess APIs.
**How to avoid:** Wrap subprocess invocation behind a platform abstraction. On Windows, use `CreateProcess`/`WaitForSingleObject`. Add `#ifdef _WIN32` guards in `build.c` or extract subprocess invocation into a `platform_spawn.h/c` abstraction.
**Warning signs:** The `iron` executable does not appear in the Windows CI matrix build output.

### Pitfall 8: Comptime Cache Stale After Function Edit
**What goes wrong:** User edits `build_sin_table()`, recompiles, but the cached result is still used because the cache key is based on old text. The binary has outdated comptime data.
**Why it happens:** The hash is computed once and not revalidated. If the cache file exists for a hash that matches the previous version, but the source changed, the hash changes and no stale hit occurs. This is actually correct behavior — BUT only if the hash covers the entire transitive dependency of the comptime function (all functions it calls). A shallow hash of just the top-level function body misses changes in callees.
**How to avoid:** For v1, hash the entire source file containing the comptime call. This is conservative (any change to any function in the file invalidates the cache) but correct. Document this limitation. A per-function hash with transitive dependency tracking is a future enhancement.

---

## Code Examples

### Comptime Evaluator Entry Point

```c
/* comptime.h */
typedef struct Iron_ComptimeVal Iron_ComptimeVal;

/* Evaluate a IRON_NODE_COMPTIME expression.
 * Returns NULL on error (diagnostics emitted).
 * On success, returns the computed value.
 * Source: comptime.h (new file this phase) */
Iron_ComptimeVal *iron_comptime_eval(Iron_Node *comptime_node,
                                      Iron_Scope *global_scope,
                                      Iron_Arena *arena,
                                      Iron_DiagList *diags,
                                      const char *source_file_dir,
                                      bool force);

/* Replace all IRON_NODE_COMPTIME nodes in the program with literals.
 * Called from iron_analyze() after type checking.
 * Source: comptime.h (new file this phase) */
void iron_comptime_apply(Iron_Program *program,
                          Iron_Scope *global_scope,
                          Iron_Arena *arena,
                          Iron_DiagList *diags,
                          const char *source_file_dir,
                          bool force_comptime);
```

### Serializing Comptime Values to C Literals

```c
/* Serialize an Iron_ComptimeVal to a C literal string.
 * Used by codegen to replace IRON_NODE_COMPTIME nodes.
 * Source: comptime.c (new file this phase) */
const char *iron_comptime_to_c_literal(const Iron_ComptimeVal *val,
                                        Iron_Arena *arena);

/* Examples of output:
 *   IRON_CVAL_INT    42          → "(int64_t)42"
 *   IRON_CVAL_FLOAT  3.14        → "3.140000"
 *   IRON_CVAL_BOOL   true        → "true"
 *   IRON_CVAL_STRING "hello"     → "iron_string_from_literal(\"hello\", 5)"
 *   IRON_CVAL_ARRAY  [1.0, 2.0]  → "{ 1.000000, 2.000000 }"
 *   IRON_CVAL_STRUCT Vec2{1,2}   → "(Iron_Vec2){ .x = 1.000000, .y = 2.000000 }"
 */
```

### extern func in gen_exprs.c

```c
/* In emit_expr, IRON_NODE_CALL case, for extern functions:
 * Source pattern: gen_exprs.c */
if (callee_id->resolved_sym &&
    callee_id->resolved_sym->is_extern) {
    /* Use extern C name (e.g. "InitWindow") not mangled Iron name */
    const char *c_name = callee_id->resolved_sym->extern_c_name;
    iron_strbuf_appendf(sb, "%s(", c_name);
    for (int i = 0; i < call->arg_count; i++) {
        if (i > 0) iron_strbuf_appendf(sb, ", ");
        /* Auto-convert Iron_String to const char* for extern params */
        Iron_Type *param_type = get_extern_param_type(callee_id->resolved_sym, i);
        if (param_type && param_type->kind == IRON_TYPE_STRING) {
            iron_strbuf_appendf(sb, "iron_string_cstr(&(");
            emit_expr(sb, call->args[i], ctx);
            iron_strbuf_appendf(sb, "))");
        } else {
            emit_expr(sb, call->args[i], ctx);
        }
    }
    iron_strbuf_appendf(sb, ")");
    break;
}
```

### draw {} Block in gen_stmts.c

```c
/* In emit_stmt, IRON_NODE_DRAW case:
 * Source pattern: gen_stmts.c */
case IRON_NODE_DRAW: {
    Iron_DrawBlock *db = (Iron_DrawBlock *)node;
    codegen_indent(sb, ctx->indent);
    iron_strbuf_appendf(sb, "BeginDrawing();\n");
    codegen_indent(sb, ctx->indent);
    iron_strbuf_appendf(sb, "{\n");
    emit_block(sb, (Iron_Block *)db->body, ctx);
    codegen_indent(sb, ctx->indent);
    iron_strbuf_appendf(sb, "}\n");
    codegen_indent(sb, ctx->indent);
    iron_strbuf_appendf(sb, "EndDrawing();\n");
    break;
}
```

### C11 Threads Abstraction in iron_runtime.h

```c
/* Threading abstraction — platform-dependent.
 * Source pattern: iron_runtime.h */
#ifdef _WIN32
  #include <threads.h>
  typedef thrd_t          iron_thread_t;
  typedef mtx_t           iron_mutex_t;
  typedef cnd_t           iron_cond_t;
  #define IRON_THREAD_CREATE(t,fn,arg)   thrd_create(&(t),(fn),(arg))
  #define IRON_THREAD_JOIN(t)            thrd_join((t), NULL)
  #define IRON_MUTEX_INIT(m)             mtx_init(&(m), mtx_plain)
  #define IRON_MUTEX_LOCK(m)             mtx_lock(&(m))
  #define IRON_MUTEX_UNLOCK(m)           mtx_unlock(&(m))
  #define IRON_MUTEX_DESTROY(m)          mtx_destroy(&(m))
  #define IRON_COND_INIT(c)              cnd_init(&(c))
  #define IRON_COND_WAIT(c,m)            cnd_wait(&(c), &(m))
  #define IRON_COND_SIGNAL(c)            cnd_signal(&(c))
  #define IRON_COND_BROADCAST(c)         cnd_broadcast(&(c))
  #define IRON_COND_DESTROY(c)           cnd_destroy(&(c))
  /* C11 thread func: int fn(void*) */
  typedef int (*iron_thread_fn)(void*);
#else
  #include <pthread.h>
  typedef pthread_t          iron_thread_t;
  typedef pthread_mutex_t    iron_mutex_t;
  typedef pthread_cond_t     iron_cond_t;
  #define IRON_THREAD_CREATE(t,fn,arg)   pthread_create(&(t),NULL,(fn),(arg))
  #define IRON_THREAD_JOIN(t)            pthread_join((t), NULL)
  #define IRON_MUTEX_INIT(m)             pthread_mutex_init(&(m), NULL)
  #define IRON_MUTEX_LOCK(m)             pthread_mutex_lock(&(m))
  #define IRON_MUTEX_UNLOCK(m)           pthread_mutex_unlock(&(m))
  #define IRON_MUTEX_DESTROY(m)          pthread_mutex_destroy(&(m))
  #define IRON_COND_INIT(c)              pthread_cond_init(&(c), NULL)
  #define IRON_COND_WAIT(c,m)            pthread_cond_wait(&(c), &(m))
  #define IRON_COND_SIGNAL(c)            cnd_signal(&(c))  /* intentional: use C11 name */
  #define IRON_COND_BROADCAST(c)         pthread_cond_broadcast(&(c))
  #define IRON_COND_DESTROY(c)           pthread_cond_destroy(&(c))
  typedef void *(*iron_thread_fn)(void*);
#endif
```

Note: The pthread worker returns `void*` but C11 thrd worker returns `int`. This means `iron_threads.c` pool_worker needs a conditional wrapper for the return type.

### Raylib Build Integration in build.c

```c
/* In invoke_clang (or a new invoke_clang_raylib variant):
 * When opts.use_raylib is true, add raylib.c to the compilation.
 * Source pattern: build.c */
if (opts.use_raylib) {
    char *rl_src = make_src_path("vendor/raylib/raylib.c");
    /* Add to argv before NULL terminator */
    /* Also add -lGL -lm -ldl -lpthread on Linux */
    /* On macOS: -framework OpenGL -framework Cocoa -framework IOKit */
    /* On Windows (clang-cl): /MD raylib.lib or compile inline */
}
```

### FNV-1a Hash for Comptime Cache Key

```c
/* Simple FNV-1a hash for cache key generation.
 * Source: comptime.c */
static uint64_t fnv1a_hash(const char *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}
```

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `IRON_NODE_COMPTIME` emitted as stub (Phase 1-3) | Replace with evaluated literal before codegen | Phase 4 | CT-01 fulfilled |
| No extern keyword | `extern func` with is_extern flag | Phase 4 | GAME-01 fulfilled |
| No draw block | `IRON_NODE_DRAW` emitted as BeginDrawing/EndDrawing | Phase 4 | GAME-02 fulfilled |
| pthreads on all platforms | Platform-abstracted threading (pthreads on Unix, C11 on Windows) | Phase 4 | RT-08/GAME-04 fulfilled |
| `build.c` uses posix_spawn only | Conditional posix_spawn (Unix) / CreateProcess (Windows) | Phase 4 | GAME-04 Windows build |

**Currently stubbed in codegen (from reading gen_exprs.c):**
- `IRON_NODE_COMPTIME`: `emit_expr(sb, ce->inner, ctx)` — passes through inner expression unchanged. Phase 4 replaces with evaluated literal.
- `IRON_NODE_RC`: emits inner expression as stub. Phase 3 runtime wired this up; the codegen stub may still need updating for full rc type emission.

---

## Open Questions

1. **`draw` keyword vs identifier**
   - What we know: `draw` is not in the current keyword list (lexer.c)
   - What's unclear: Whether adding `draw` as a keyword breaks any existing integration tests that might use `draw` as a variable name
   - Recommendation: Check integration tests before adding; add `draw` to keyword list in lexer alongside other game-dev keywords

2. **raylib Vec2 field types: Float vs Float32**
   - What we know: Raylib's `Vector2` uses C `float` (32-bit); Iron's `Float` maps to `double` (64-bit)
   - What's unclear: Whether the Vec2 object in raylib.iron should use `Float32` (matching raylib) or `Float` (ergonomic for users who might lose precision silently)
   - Recommendation: Use `Float32` in the raylib.iron wrapper — correct layout is mandatory for FFI; users who need double precision should use their own Vec2 type

3. **Comptime restriction enforcement: type checker or interpreter**
   - What we know: The type checker already has a `check_expr` dispatch; the interpreter also walks the AST
   - What's unclear: Whether restrictions (no heap, no rc) are enforced at the type checker stage (before evaluation) or in the interpreter at runtime
   - Recommendation: Enforce in both: the type checker emits an error for `heap` inside a function that is called via `comptime` (requires tracking "currently in comptime context" flag in TypeCtx); the interpreter also errors as a belt-and-suspenders check. Type checker enforcement catches the error without running the interpreter.

4. **Comptime across multiple files**
   - What we know: The current build pipeline processes one file at a time; comptime evaluator needs to call user functions
   - What's unclear: If `main.iron` does `comptime build_table()` and `build_table` is defined in `utils.iron`, does the comptime evaluator have access to utils.iron's AST?
   - Recommendation: For v1, the comptime evaluator runs after the full multi-file analysis (the global scope has all symbols), so the function's FuncDecl is accessible via `global_scope` lookup. This works for the current single-file build pipeline. Multi-file comptime will work if the resolver has already merged all modules into the global scope.

5. **clang-cl subprocess on macOS/Linux CI**
   - What we know: The CI will add `windows-latest`; the macOS/Linux builds continue using `posix_spawn`
   - What's unclear: Whether `build.c` needs to detect the platform at runtime or compile time for subprocess API selection
   - Recommendation: Compile-time detection via `#ifdef _WIN32` in `build.c`. The macOS/Linux builds use the existing `posix_spawn` path unchanged. The Windows build path uses `CreateProcess`. This is simpler than runtime detection.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Unity 2.6.1 (via CMake FetchContent) |
| Config file | CMakeLists.txt |
| Quick run command | `ctest --test-dir build -R "test_comptime\|test_raylib\|test_extern"` |
| Full suite command | `ctest --test-dir build` |

**Current baseline:** 20 tests pass (verified 2026-03-25).

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CT-01 | `comptime` eval produces C literals | unit | `ctest --test-dir build -R test_comptime` | ❌ Wave 2 |
| CT-01 | `comptime build_sin_table()` integration | integration | `run_integration.sh iron comptime_sin.iron` | ❌ Wave 2 |
| CT-02 | `comptime read_file()` embeds file contents | unit | `ctest --test-dir build -R test_comptime` | ❌ Wave 3 |
| CT-03 | No heap/rc in comptime → error | unit | `ctest --test-dir build -R test_comptime` | ❌ Wave 2 |
| CT-04 | Step limit triggers compile error | unit | `ctest --test-dir build -R test_comptime` | ❌ Wave 2 |
| GAME-01 | `extern func` calls emit raw C name | unit | `ctest --test-dir build -R test_codegen` | ❌ Wave 1 |
| GAME-01 | `import raylib` + `init_window` compiles | integration | `run_integration.sh iron raylib_window.iron` | ❌ Wave 1 |
| GAME-02 | `draw {}` lowers to BeginDrawing/EndDrawing | unit | `ctest --test-dir build -R test_codegen` | ❌ Wave 1 |
| GAME-03 | Binary is standalone (no Iron install needed) | integration | existing test framework verifies this | ✅ (existing) |
| GAME-04 | Windows binary builds with clang-cl | CI | GitHub Actions windows-latest matrix | ❌ Wave 4 |
| RT-08 | Runtime tests pass on Windows | CI | GitHub Actions windows-latest ctest | ❌ Wave 4 |

### Sampling Rate
- **Per task commit:** `ctest --test-dir build` (20 tests, ~8 seconds)
- **Per wave merge:** `ctest --test-dir build` + review integration test output
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/test_comptime.c` — covers CT-01, CT-02, CT-03, CT-04
- [ ] `tests/integration/raylib_window.iron` — covers GAME-01 (raylib window creation)
- [ ] `tests/integration/comptime_sin.iron` + expected output — covers CT-01 (sin table)
- [ ] `tests/integration/draw_block.iron` + expected output — covers GAME-02
- [ ] `.github/workflows/ci.yml` — Windows CI matrix (covers GAME-04, RT-08)

*(Note: `tests/test_codegen.c` already exists and can be extended for extern/draw tests without a new file.)*

---

## Sources

### Primary (HIGH confidence)
- Source code read directly: `src/codegen/codegen.c`, `src/codegen/gen_exprs.c`, `src/codegen/gen_stmts.c`, `src/codegen/gen_types.c` — current codegen patterns and stubs
- Source code read directly: `src/cli/build.c` — existing clang invocation and build pipeline
- Source code read directly: `src/runtime/iron_runtime.h`, `src/runtime/iron_threads.c` — current pthreads implementation
- Source code read directly: `src/parser/ast.h` — existing node types including IRON_NODE_COMPTIME
- `.planning/phases/04-comptime-game-dev-and-cross-platform/04-CONTEXT.md` — all locked decisions
- `.planning/REQUIREMENTS.md` — CT-01..CT-04, GAME-01..GAME-04, RT-08 definitions
- `.planning/research/PITFALLS.md` — Pitfall 7 (comptime step limit), Pitfall 11 (Windows threads), Pitfall 14 (read_file paths)

### Secondary (MEDIUM confidence)
- `docs/language_definition.md` — comptime syntax, extern keyword, draw block syntax, raylib wrapper contract
- `docs/implementation_plan.md` — Phase 8 comptime approach, Phase 4 type mapping
- Build verification: all 20 tests pass on current codebase (verified 2026-03-25)

### Tertiary (LOW confidence)
- C11 threads on Windows MSVC: https://devblogs.microsoft.com/cppblog/c11-threads-in-visual-studio-2022-version-17-8-preview-2/ — confirms C11 `<threads.h>` available on MSVC 17.8+
- TinyCThread as C11 fallback: https://tinycthread.github.io/ — portable shim if needed

---

## Metadata

**Confidence breakdown:**
- Comptime design: HIGH — CONTEXT.md has all locked decisions; existing IRON_NODE_COMPTIME stub is in gen_exprs.c
- extern func / draw block: HIGH — implementation pattern is clear from existing codegen patterns in gen_exprs.c/gen_stmts.c
- Raylib integration: HIGH — bundled raylib.c approach is well-understood; Vec2 layout is a known FFI pitfall (documented in research)
- Windows parity: MEDIUM — C11 threads abstraction design is clear; clang-cl subprocess needs verification against actual Windows environment
- Comptime cache: MEDIUM — FNV-1a hash + JSON file approach is straightforward but the transitive dependency limitation must be documented

**Research date:** 2026-03-25
**Valid until:** 2026-04-25 (raylib stable; C11 threads stable)
