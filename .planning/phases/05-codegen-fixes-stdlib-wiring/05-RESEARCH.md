# Phase 5: Codegen Fixes + Stdlib Wiring - Research

**Researched:** 2026-03-26
**Domain:** C codegen (string interpolation, parallel-for), Iron stdlib wiring (.iron wrapper files + resolver + build pipeline)
**Confidence:** HIGH — all findings are based on direct source inspection of the codebase

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**String Interpolation Codegen**
- snprintf-based formatting: `"value is {x}"` emits C code using snprintf with format specifiers per expression type
- `iron_to_cstring()` runtime helper: all types go through a single conversion function — Int, Float, Bool, String handled natively; objects with a `to_string() -> String` method call that method; objects without `to_string()` are a compile error in interpolation
- Full expressions supported inside `{}` — arithmetic, method calls, field access all valid (parser already handles sub-parser for {expr} segments)
- Float formatting uses `%g` (trims trailing zeros): 3.14159 stays 3.14159, 3.0 shows as 3
- Two-pass snprintf allocation: first `snprintf(NULL, 0, ...)` to measure length; if fits in `char[1024]` stack buffer use that, else malloc exact size; then `iron_string_from_literal()` to wrap as Iron_String
- Escaped braces supported: `\{` and `\}` emit literal `{` and `}` (lexer already detects interpolation via unescaped `{` only)

**Stdlib Import Wiring**
- Wrapper .iron files: `math.iron`, `io.iron`, `time.iron`, `log.iron` in `src/stdlib/` — same pattern as `raylib.iron`
- Module-qualified access: `import math` then `Math.sin(1.0)`, `Math.PI` — NOT bare names
- Stdlib modules are Iron objects with auto-static methods: `object Math { func sin(x: Float) -> Float { ... } }` where methods that don't reference `self` or instance `var` fields are automatically callable as static: `Math.sin(1.0)`
- **Auto-static is a general language feature** — works for ALL objects, not just stdlib. Any method without `self`/instance-var references is callable on the type directly: `MyUtils.helper()`
- PascalCase object names: `Math`, `IO`, `Time`, `Log` — matches Iron object naming convention
- Constants as static val fields: `Math.PI`, `Math.TAU`, `Math.E` — auto-static applies to vals too
- RNG as nested object in Math: `Math.RNG(seed)` creates an instance with methods `rng.next()`, `rng.next_int(min, max)`, `rng.next_float()`. Global convenience: `Math.random()`, `Math.random_int(min, max)`
- LogLevel enum in log.iron: `Log.set_level(LogLevel.DEBUG)`. Levels: DEBUG, INFO, WARN, ERROR
- Multi-return works with static methods: `val content, err = IO.read_file("test.txt")` returns `(String, Error)` tuple via anonymous result struct (existing Phase 3 codegen)
- Import detection: `strstr` in `build.c` (same as `import raylib`) detects `import math` etc. and prepends the corresponding .iron wrapper file before lexing

**Parallel-For Codegen**
- Context struct for captured variables: codegen detects outer variable references in the for body, generates a struct with pointers to captured vars, passes as `void* ctx_arg` (slot already exists in chunk function signature)
- Implicit barrier: `parallel for` always blocks until all chunks complete — code after the block can safely read results
- Array indexing allowed: `array[i] = compute(i)` is safe because each iteration writes to a distinct index — concurrency checker permits this alongside its existing "no mutation of outer non-mutex vars" rule
- Panic propagation: first panic sets a shared error flag; other chunks check flag at iteration start and bail; after barrier, panic is re-raised on the main thread (matches spawn/await panic model)

**Integration Testing**
- Same pattern as Phase 3: `iron build` -> execute binary -> compare stdout to `.expected` file
- Parallel-for verification: both aggregate (sum check for deterministic result) and sorted-output comparison for comprehensive verification
- Both C unit tests AND .iron integration tests: C unit tests verify codegen emits correct C patterns (snprintf, chunk functions); .iron tests verify end-to-end behavior
- One test file per stdlib module: `test_math.iron`, `test_io.iron`, `test_time.iron`, `test_log.iron`
- Combined integration test: one .iron file exercising interpolation + stdlib + parallel together
- Update existing `hello.iron` to use string interpolation as a smoke test
- Separate tests per feature + combined end-to-end test for maximum coverage

### Claude's Discretion
- Exact `iron_to_cstring()` implementation details (buffer management, type dispatch)
- Auto-static detection implementation in semantic analysis (how to flag methods)
- Parallel-for capture struct naming convention
- Panic flag implementation details (atomic bool, shared struct)
- Exact C unit test structure for new codegen paths
- How to wire the `.iron` wrapper file prepend order when multiple stdlib imports are present

### Deferred Ideas (OUT OF SCOPE)
None — discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| GEN-01 | C code emitted for all Iron language constructs compiles with `gcc/clang -std=c11 -Wall -Werror` | String interpolation snprintf codegen + capture struct codegen must produce valid C11 |
| GEN-11 | Parallel-for generates range splitting, chunk submission, and barrier | Existing parallel-for in gen_stmts.c needs capture struct addition and correct start/end passing |
| STD-01 | math module provides trig, sqrt, pow, lerp, random, PI/TAU/E | iron_math.h C API verified; need math.iron wrapper + build.c strstr detection |
| STD-02 | io module provides file read/write, file_exists, list_files, create_dir | iron_io.h C API verified; need io.iron wrapper + build.c strstr detection |
| STD-03 | time module provides now, now_ms, sleep, since, Timer | iron_time.h C API verified; need time.iron wrapper + build.c strstr detection |
| STD-04 | log module provides info/warn/error/debug with level filtering | iron_log.h C API verified; need log.iron wrapper + build.c strstr detection |
</phase_requirements>

---

## Summary

Phase 5 is a correctness and wiring phase — it fixes two known stubs in the codegen and exposes four existing C stdlib libraries as importable Iron modules. All C implementations already exist and compile. There is no net-new algorithmic work; the work is connecting existing pieces correctly.

The three work streams are independent of each other and can be planned as parallel tracks: (1) string interpolation codegen in `gen_exprs.c`, (2) parallel-for capture struct fix in `gen_stmts.c`, and (3) stdlib wiring across four files (`math.iron`, `io.iron`, `time.iron`, `log.iron`) plus resolver and build pipeline changes.

The hardest integration point is auto-static: the resolver and typechecker must recognize method calls on type names (not instances) and route them to the correct C function. The existing IRON_NODE_METHOD_CALL path handles instance calls; type-level calls currently fall through to IRON_NODE_CALL with a IRON_SYM_TYPE callee — that path needs to detect static method dispatch and emit `Iron_Math_sin(arg)` rather than a constructor.

**Primary recommendation:** Implement in three independent plans — string interpolation, parallel-for fix, stdlib wiring — with a combined integration test plan at the end.

---

## Standard Stack

### Core (all verified by direct source inspection)

| Component | File | Current State | What Phase 5 Changes |
|-----------|------|--------------|----------------------|
| gen_exprs.c | `src/codegen/gen_exprs.c:291` | IRON_NODE_INTERP_STRING emits `""` stub | Replace stub with snprintf two-pass emission |
| gen_stmts.c | `src/codegen/gen_stmts.c:243` | Parallel-for chunk passes `(void*)_c` (the start value) but chunk function ignores ctx_arg | Add capture struct; fix start/end passing |
| iron_math.h | `src/stdlib/iron_math.h` | C API fully implemented | Add math.iron wrapper + build.c detection |
| iron_io.h | `src/stdlib/iron_io.h` | C API fully implemented | Add io.iron wrapper + build.c detection |
| iron_time.h | `src/stdlib/iron_time.h` | C API fully implemented | Add time.iron wrapper + build.c detection |
| iron_log.h | `src/stdlib/iron_log.h` | C API fully implemented | Add log.iron wrapper + build.c detection |
| resolve.c | `src/analyzer/resolve.c:143` | IRON_NODE_IMPORT_DECL defines alias as placeholder IRON_SYM_TYPE only if alias present | Register module object (Math/IO/Time/Log) as IRON_SYM_TYPE with decl_node pointing to object |
| build.c | `src/cli/build.c:548` | Handles `import raylib` strstr + prepend | Add strstr for `import math`, `import io`, `import time`, `import log` |

### Supporting

| Component | File | Purpose |
|-----------|------|---------|
| iron_runtime.h | `src/runtime/iron_runtime.h` | Iron_String, iron_string_from_cstr/from_literal for snprintf output wrapping |
| raylib.iron | `src/stdlib/raylib.iron` | Template pattern for new .iron wrappers |
| test_codegen.c | `tests/test_codegen.c` | Template for new codegen unit tests |
| test_stdlib.c | `tests/test_stdlib.c` | Existing stdlib C-level test (does NOT test Iron-level integration) |
| run_integration.sh | `tests/integration/run_integration.sh` | Integration test runner — auto-discovers `.iron` + `.expected` pairs |

---

## Architecture Patterns

### Pattern 1: snprintf Two-Pass String Interpolation

The IRON_NODE_INTERP_STRING AST node contains segments (literal strings and sub-expressions). The codegen replacement in `gen_exprs.c` case `IRON_NODE_INTERP_STRING`:

```c
// Source: gen_exprs.c IRON_NODE_INTERP_STRING case (to replace stub)
// Two-pass: measure, allocate/stack-alloc, format, wrap as Iron_String

// 1. Build format string and args list at code-generation time by inspecting
//    each segment's type (IRON_TYPE_INT -> %lld, IRON_TYPE_FLOAT -> %g,
//    IRON_TYPE_BOOL -> %s, IRON_TYPE_STRING -> %s via iron_string_cstr())
// 2. Emit a GNU statement expression ({ ... }) so the result is an rvalue:
//    ({ char _buf[1024]; int _n = snprintf(_buf, 1024, "value is %lld", x);
//       Iron_String _s;
//       if (_n < 1024) { _s = iron_string_from_cstr(_buf, (size_t)_n); }
//       else {
//           char *_hp = malloc((size_t)_n + 1);
//           snprintf(_hp, (size_t)_n + 1, "value is %lld", x);
//           _s = iron_string_from_cstr(_hp, (size_t)_n);
//           free(_hp);
//       }
//       _s; })
```

Type-to-format-specifier mapping (decided):
- `IRON_TYPE_INT` → `%lld` with `(long long)` cast
- `IRON_TYPE_FLOAT` → `%g`
- `IRON_TYPE_BOOL` → `%s` with `(expr ? "true" : "false")`
- `IRON_TYPE_STRING` → `%s` with `iron_string_cstr(&(expr))`
- Object with `to_string()` → call `to_string()`, then `%s` via `iron_string_cstr()`

The `iron_to_cstring()` helper (Claude's discretion for implementation) can be a static inline or a separate runtime function. Given it needs to call Iron methods dynamically for objects, it should be emitted by codegen at the call site (not a runtime function) — the codegen inspects the type and emits the appropriate C expression inline.

### Pattern 2: Parallel-For Capture Struct

The existing parallel-for in `gen_stmts.c:243` has a critical bug: it passes `(void*)_c` (the loop start value cast to a pointer) to `Iron_pool_submit`, but the chunk function receives `void* ctx_arg` and ignores it with `(void)ctx_arg`. The start/end splitting is computed correctly but never passed to the chunk function.

Fix approach (matches CONTEXT.md decisions):

```c
// Capture struct generated per parallel-for site:
typedef struct {
    int64_t start;
    int64_t end;
    // ...captured outer vars as pointers...
    int64_t *array;   // example: array captured by pointer
} Iron_parallel_ctx_0;

// Chunk function receives ctx_arg as Iron_parallel_ctx_0*:
void Iron_parallel_chunk_0(void* ctx_arg) {
    Iron_parallel_ctx_0 *ctx = (Iron_parallel_ctx_0*)ctx_arg;
    int64_t start = ctx->start;
    int64_t end = ctx->end;
    int64_t *array = ctx->array;
    for (int64_t i = start; i < end; i++) {
        array[i] = i;
    }
}

// At use site: allocate one ctx per chunk, submit:
int64_t _total = 100;
int64_t _nthreads = Iron_pool_thread_count(Iron_global_pool);
int64_t _chunk_size = (_total + _nthreads - 1) / _nthreads;
for (int64_t _c = 0; _c < _total; _c += _chunk_size) {
    int64_t _end = (_c + _chunk_size > _total) ? _total : _c + _chunk_size;
    Iron_parallel_ctx_0 *_ctx = malloc(sizeof(Iron_parallel_ctx_0));
    _ctx->start = _c;
    _ctx->end = _end;
    _ctx->array = array;
    Iron_pool_submit(Iron_global_pool, Iron_parallel_chunk_0, _ctx);
}
Iron_pool_barrier(Iron_global_pool);
// Note: ctx structs are freed inside the chunk function after use
```

**Capture detection reuse:** The existing `collect_captures()` in `gen_exprs.c` already walks AST nodes for lambdas. The same function can be used for parallel-for bodies (pass `fs->var_name` as the "param" to exclude from captures).

### Pattern 3: Stdlib .iron Wrapper Files

Follow `raylib.iron` pattern exactly. Key differences from raylib:
- No extern funcs — use Iron `object` with auto-static methods that call underlying C functions
- Auto-static works because methods don't reference `self` or instance `var` fields
- Constants become `val` fields on the object

```iron
-- math.iron — Iron wrapper for the Math stdlib module
object Math {
  -- Constants (auto-static: accessible as Math.PI)
  val PI:  Float = 3.14159265358979323846
  val TAU: Float = 6.28318530717958647692
  val E:   Float = 2.71828182845904523536

  -- Methods (auto-static: no self/instance-var access)
  func sin(x: Float) -> Float { ... }
  func cos(x: Float) -> Float { ... }
  -- etc.
}
```

The methods inside the object body call the C functions. Since Iron compiles through C, the method body can emit `Iron_math_sin(x)` directly. The implementation of these methods uses `extern func` under the hood OR the codegen for method bodies that contain only a single extern call can be handled specially. The cleaner approach: declare the C functions as `extern func` within the object scope (or module-level) and call them from the methods.

Simpler approach (preferred, matches Phase 4 extern func pattern): The method bodies in math.iron, etc. are Iron functions that wrap extern calls. Each wrapper object method has a body that calls the corresponding C function via extern func declaration in the same file.

### Pattern 4: Build Pipeline Import Detection

The `iron_build()` function in `build.c:548` already handles `import raylib` with strstr + prepend. Extend this pattern for each stdlib module:

```c
// After the existing raylib detection block (build.c ~line 548):
if (strstr(source, "import math") != NULL) {
    char *math_path = make_src_path("stdlib/math.iron");
    // prepend math.iron source to source buffer
    // (same logic as raylib prepend)
}
// Same pattern for import io, import time, import log
```

Multiple stdlib imports in the same file: prepend all detected ones before lexing. Order: math, io, time, log, (user source). Since the .iron wrapper files only declare types/objects, order doesn't matter for correctness, but a consistent order avoids confusion.

No changes needed to `build_src_list()` — the stdlib C files (`iron_math.c`, `iron_io.c`, `iron_time.c`, `iron_log.c`) are **already unconditionally linked** into every compiled binary (verified at `build.c:303-306`). This means the C implementations are always available; the .iron wrappers only need to expose the API surface to Iron source.

### Pattern 5: Auto-Static Dispatch in Codegen

When the user writes `Math.sin(1.0)`, the parser produces a FIELD_ACCESS `Math.sin` as the callee of a CALL node, or an IRON_NODE_METHOD_CALL where the receiver is the type name `Math` (an IRON_SYM_TYPE symbol). The codegen must detect this case.

Current IRON_NODE_METHOD_CALL codegen (`gen_exprs.c:525`):
```c
// Gets type_name from mc->object's resolved_type.kind == IRON_TYPE_OBJECT
// Then emits: Iron_Math_sin(&math_instance, 1.0)
```

For auto-static, the receiver is the type name itself (no instance). The codegen check:
```c
// In IRON_NODE_METHOD_CALL:
if (mc->object->kind == IRON_NODE_IDENT) {
    Iron_Ident *obj_id = (Iron_Ident *)mc->object;
    if (obj_id->resolved_sym &&
        obj_id->resolved_sym->sym_kind == IRON_SYM_TYPE) {
        // Auto-static call: emit Iron_Math_sin(args) — no self pointer
        const char *mangled = iron_mangle_method(
            obj_id->name, mc->method, ctx->arena);
        iron_strbuf_appendf(sb, "%s(", mangled);
        for (int i = 0; i < mc->arg_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            emit_expr(sb, mc->args[i], ctx);
        }
        iron_strbuf_appendf(sb, ")");
        break;
    }
}
```

The semantic analysis must also handle this: the typechecker needs to resolve `Math.sin` as a valid call on the type (not an error). The method symbol lookup in typecheck.c currently requires `self` pointer. For auto-static, the typecheck path detects that the method body has no `self` usage and marks it callable without instance (flag on `Iron_Symbol` or `Iron_MethodDecl`).

### Recommended Project Structure for New Files

```
src/stdlib/
├── math.iron          # NEW — Math object wrapper
├── io.iron            # NEW — IO object wrapper
├── time.iron          # NEW — Time object wrapper
├── log.iron           # NEW — Log object wrapper
├── iron_math.h        # EXISTING — C API (verified present)
├── iron_io.h          # EXISTING — C API (verified present)
├── iron_time.h        # EXISTING — C API (verified present)
├── iron_log.h         # EXISTING — C API (verified present)

tests/integration/
├── test_interp.iron       # NEW — string interpolation end-to-end
├── test_interp.expected   # NEW
├── test_parallel.iron     # NEW — parallel-for correctness
├── test_parallel.expected # NEW
├── test_math.iron         # NEW — Math module
├── test_math.expected     # NEW
├── test_io.iron           # NEW — IO module
├── test_io.expected       # NEW
├── test_time.iron         # NEW — Time module
├── test_time.expected     # NEW
├── test_log.iron          # NEW — Log module
├── test_log.expected      # NEW
├── test_combined.iron     # NEW — interpolation + stdlib + parallel together
├── test_combined.expected # NEW
├── hello.iron             # MODIFIED — add string interpolation smoke test
```

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| String length measurement | Custom strlen loop | `snprintf(NULL, 0, fmt, ...)` returns needed length | Standard C11, zero overhead |
| Stack vs heap string buffer decision | Manual threshold logic | 1024-byte stack buffer threshold (decided) | Already decided; consistent with SSO threshold |
| Iron_String wrapping of C char* | Custom struct init | `iron_string_from_cstr(buf, len)` | Handles SSO/heap path automatically |
| Thread-local RNG for `Math.random()` | Custom thread-local state | `Iron_math_random()` already implemented with `__thread` state | Already in iron_math.c |
| Stdlib C linking | Conditional link flags | Already unconditionally linked in `build_src_list()` | No build change needed |
| Capture collection for parallel-for body | New AST walker | Reuse `collect_captures()` from gen_exprs.c | Already handles DFS over all relevant node kinds |

**Key insight:** The C stdlib implementations are fully production-ready and already linked. Phase 5's value is entirely in the Iron-language exposure layer.

---

## Common Pitfalls

### Pitfall 1: Parallel-For Start/End Bug (KNOWN BUG — MUST FIX)

**What goes wrong:** The current codegen at `gen_stmts.c:291-292` calls:
```c
Iron_pool_submit(Iron_global_pool, (void(*)(void*))chunk_name, (void*)_c);
```
This casts the integer `_c` (the chunk start) to a `void*` and passes it as the work argument. But the chunk function signature is `void chunk(int64_t start, int64_t end, void* ctx_arg)` — this is a 3-argument function, not a 1-argument `void(*)(void*)`. The cast is wrong AND the chunk function never receives start/end correctly.

**Root cause:** The chunk function was originally designed with a 3-argument signature but `Iron_pool_submit` expects `void(*)(void*)`. These are incompatible.

**How to fix:** Change chunk function signature to `void chunk(void* ctx_arg)` and unpack start/end from the capture struct. This makes it compatible with `Iron_pool_submit`.

**Warning signs:** Parallel-for produces incorrect results or crashes immediately.

### Pitfall 2: Auto-Static Breaks Existing Method Dispatch

**What goes wrong:** Adding auto-static detection in the typechecker/codegen could accidentally match regular method calls where the object variable happens to have the same name as a type.

**How to avoid:** Auto-static triggers ONLY when `resolved_sym->sym_kind == IRON_SYM_TYPE` on the receiver ident, not on `IRON_SYM_VAR` or `IRON_SYM_FUNCTION`. The resolved_sym check is already done in the CALL case (line ~474 in gen_exprs.c). Apply same check in METHOD_CALL.

### Pitfall 3: snprintf Format String Injection

**What goes wrong:** If a string interpolation segment contains a string value with `%` characters, passing it as a `%s` arg to snprintf is safe (the content goes into the arg, not the format string). The format string is always a compile-time-constructed string literal in the generated C — no user data in the format string.

**How to avoid:** Always construct the format string at code-generation time from the segment types, never from segment values.

### Pitfall 4: Multiple stdlib Import Prepend Order

**What goes wrong:** If `io.iron` references `Iron_Error` (which is defined in `iron_runtime.h` included by the C files) but the Iron type system needs to know about `Error` type, the .iron wrappers may need to declare it. However, `Iron_Error` is a C-level struct, and the Iron multi-return pattern uses `Iron_Result_String_Error` which is also C-level.

**How to avoid:** Stdlib wrapper methods that return multi-value results (like `IO.read_file`) should use multi-return syntax `-> (String, Error)` in Iron, which maps to `Iron_Result_String_Error` through the existing anonymous result struct codegen from Phase 3. Verify that `Iron_Error`/`Error` is registered in the resolver's global scope before the stdlib .iron files are processed.

### Pitfall 5: `Math.PI` as Object Val Field vs Constant

**What goes wrong:** An object `val` field generates a struct member in C (`Iron_Math.PI`). But auto-static access to a `val` field would require emitting `Iron_Math_PI` as a global constant, not accessing a struct field.

**How to avoid:** Constants in stdlib modules should be declared as module-level `val` declarations (not as fields inside the object body), OR the codegen for field access on a type name (not instance) should emit the constant directly. The cleaner approach: declare `PI`, `TAU`, `E` as top-level vals in math.iron (not inside the object body), which the codegen emits as C global constants. Access as `Math.PI` would then need to be handled by a module namespace convention. Alternatively, treat them as `extern val` constants pointing to `#define IRON_PI` in C.

**Recommendation:** Declare math constants as module-level `val` declarations in math.iron and register them as `Math.PI` etc. in the resolver under the `Math` namespace. This is simpler than auto-static val fields.

### Pitfall 6: `collect_captures()` Missing Array Index Targets

**What goes wrong:** The capture collector in gen_exprs.c uses a DFS that stops at certain node types (the default case does nothing). `IRON_NODE_INDEX` (array indexing) and `IRON_NODE_FIELD_ACCESS` are in the switch default, so their sub-expressions aren't walked.

**How to avoid:** When extending `collect_captures()` for parallel-for use, add cases for IRON_NODE_INDEX, IRON_NODE_FIELD_ACCESS, and IRON_NODE_METHOD_CALL to ensure outer variables accessed through indexing or field access are captured correctly.

---

## Code Examples

### String Interpolation AST Node Structure

The parser already produces IRON_NODE_INTERP_STRING. Looking at `gen_exprs.c:291`, the case stub shows the node type exists. The AST node (from `parser/ast.h`) has segments — need to verify the field name:

```c
// From parser/ast.h (need to verify exact field names):
// Iron_InterpString likely has:
//   Iron_Node **segments;  (alternating literal and expr nodes)
//   int segment_count;
// OR:
//   Iron_InterpSegment *segments;
//   where each segment has: bool is_expr; char *literal; Iron_Node *expr;
```

**Action required in plan:** Read `src/parser/ast.h` to confirm IRON_NODE_INTERP_STRING struct layout before implementing.

### Raylib.iron Pattern (Template for New Wrappers)

The raylib.iron file structure (verified):
```iron
-- Types: object declarations
-- Constants: val declarations
-- Enums: enum declarations
-- Functions: extern func declarations
```

For stdlib wrappers, the pattern shifts: instead of `extern func`, the wrapper uses `object` with auto-static methods that are themselves declared as `extern func` calls internally, OR the method bodies contain a single line calling the C function by its Iron name.

The simplest approach given the existing extern func machinery: declare the underlying C functions as `extern func` within the wrapper file (same as raylib does for C APIs), then have the math/io/time/log objects be thin shims. However, the decision says "Iron objects with auto-static methods where methods that don't reference `self`" — this means actual Iron method bodies, not extern func wrappers.

**Resolution:** The method bodies call the C stdlib functions. Since extern func wires to the C ABI directly, a method body `{ return Iron_math_sin(x) }` would use the C name directly. The codegen for normal method calls emits `Iron_Math_sin(&self, x)` — the auto-static path emits `Iron_Math_sin(x)`. This means the C function `Iron_math_sin` and the generated C method `Iron_Math_sin` would collide.

**The correct approach:** Declare the underlying C functions as extern funcs in the .iron wrapper (using their exact C names), and have the object methods call those extern funcs. Example:

```iron
-- math.iron
extern func iron_math_sin(x: Float) -> Float
extern func iron_math_cos(x: Float) -> Float
-- ... etc.

object Math {
  func sin(x: Float) -> Float { return iron_math_sin(x) }
  func cos(x: Float) -> Float { return iron_math_cos(x) }
}
```

This correctly separates the C-level name from the Iron-level name, avoids symbol collision, and uses the existing extern func codegen (which emits raw C names without Iron_ prefix mangling for extern funcs).

### Integration Test Pattern (Verified from hello.iron + run_integration.sh)

```iron
-- test_interp.iron
func main() {
  val x = 42
  val msg = "value is {x}"
  println(msg)
}
```

```
-- test_interp.expected
value is 42
```

The test runner auto-discovers `.iron` + `.expected` pairs. No test registration needed.

---

## State of the Art

| Old Approach | Current Approach | Impact |
|--------------|------------------|--------|
| Empty string stub for interpolation | snprintf two-pass | Makes interpolation actually work |
| `(void*)_c` passed to pool_submit | Capture struct with start/end | Makes parallel-for actually work |
| Stdlib C functions exist but unexposed | .iron wrappers + import detection | Makes `import math` etc. work |

**Known stubs in current codebase:**
- `gen_exprs.c:291`: `IRON_NODE_INTERP_STRING` emits `""` (verified)
- `gen_stmts.c:292`: parallel-for passes wrong argument type to `Iron_pool_submit` (verified — `(void(*)(void*))chunk_name` cast with 3-arg function, and passes start value not a context struct)
- `resolve.c:143`: `IRON_NODE_IMPORT_DECL` only registers alias if present; does not register the module object in scope (verified)

---

## Open Questions

1. **IRON_NODE_INTERP_STRING struct layout**
   - What we know: The node kind exists and the parser handles it (per STATE.md "01-04: String interpolation uses sub-parser for {expr} segments")
   - What's unclear: Exact field names (`segments`, `segment_count`, `parts`, etc.)
   - Recommendation: Read `src/parser/ast.h` at plan time to confirm before writing interpolation codegen

2. **Auto-static method detection in typecheck.c**
   - What we know: The typechecker validates method calls; adding auto-static means allowing `TypeName.method()` without an instance
   - What's unclear: Whether the typechecker currently errors on `Math.sin(1.0)` as "calling method on type not instance" before codegen ever runs
   - Recommendation: Check `src/analyzer/typecheck.c` METHOD_CALL handling; add auto-static path that checks if receiver resolved_sym is IRON_SYM_TYPE and the method exists on that type

3. **Iron_Error type in resolver scope**
   - What we know: `Iron_Error` is a C struct (`iron_runtime.h:100`). `IO.read_file` returns `(String, Error)`.
   - What's unclear: Whether `Error` is registered as an Iron type in the resolver global scope, or only exists at C level
   - Recommendation: Check `src/analyzer/resolve.c` for built-in type registration; may need to register `Error` as a built-in primitive type alongside `Int`, `Float`, `Bool`, `String`

4. **Iron_Pool_submit signature vs chunk function**
   - What we know: `Iron_pool_submit(pool, void(*fn)(void*), void* arg)` from `iron_runtime.h:134`
   - What's unclear: The existing chunk function has signature `void chunk(int64_t start, int64_t end, void* ctx_arg)` which is NOT compatible with `void(*)(void*)`
   - Recommendation: Change chunk function signature to `void chunk(void* ctx_arg)` and unpack start/end from ctx struct — this is the correct fix per CONTEXT.md decisions

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Unity (verified in test_codegen.c, test_stdlib.c) |
| Config file | CMakeLists.txt — add_test() per test binary |
| Quick run command | `cd /Users/victor/code/iron-lang/build && ctest -R test_codegen --output-on-failure` |
| Full suite command | `cd /Users/victor/code/iron-lang/build && ctest --output-on-failure` |
| Integration tests | `tests/integration/run_integration.sh ./build/iron` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| GEN-01 | String interpolation produces correct C11 | unit | `ctest -R test_codegen -k interp` | ❌ Wave 0 |
| GEN-11 | Parallel-for chunk receives correct start/end via ctx struct | unit | `ctest -R test_codegen -k parallel` | ❌ Wave 0 |
| GEN-11 | `parallel for i in range(100)` produces sum 4950 | integration | `tests/integration/run_integration.sh` | ❌ Wave 0 |
| GEN-01 | `"value is {x}"` where x=42 outputs "value is 42" | integration | `tests/integration/run_integration.sh` | ❌ Wave 0 |
| STD-01 | `Math.sin(1.0)` compiles and returns ~0.841 | integration | `tests/integration/run_integration.sh` | ❌ Wave 0 |
| STD-02 | `IO.read_file("test.txt")` returns contents | integration | `tests/integration/run_integration.sh` | ❌ Wave 0 |
| STD-03 | `Time.now_ms()` returns a positive int | integration | `tests/integration/run_integration.sh` | ❌ Wave 0 |
| STD-04 | `Log.info(...)` writes to stderr without crashing | integration | `tests/integration/run_integration.sh` | ❌ Wave 0 |

### Sampling Rate
- **Per task commit:** `cd build && ctest -R test_codegen --output-on-failure -j4`
- **Per wave merge:** `cd build && ctest --output-on-failure -j4 && tests/integration/run_integration.sh`
- **Phase gate:** Full suite green + all integration tests pass before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `tests/test_codegen.c` — add test cases for IRON_NODE_INTERP_STRING snprintf emission and parallel-for capture struct emission
- [ ] `tests/integration/test_interp.iron` + `.expected` — string interpolation end-to-end
- [ ] `tests/integration/test_parallel.iron` + `.expected` — parallel-for correctness (sum check)
- [ ] `tests/integration/test_math.iron` + `.expected` — Math module callable
- [ ] `tests/integration/test_io.iron` + `.expected` — IO module callable
- [ ] `tests/integration/test_time.iron` + `.expected` — Time module callable
- [ ] `tests/integration/test_log.iron` + `.expected` — Log module callable
- [ ] `tests/integration/test_combined.iron` + `.expected` — combined end-to-end
- [ ] Update `tests/integration/hello.iron` — add string interpolation smoke test

---

## Sources

### Primary (HIGH confidence — direct source inspection)
- `src/codegen/gen_exprs.c` — IRON_NODE_INTERP_STRING stub at line 291; existing emission patterns; collect_captures() DFS walker
- `src/codegen/gen_stmts.c` — Parallel-for codegen at line 243; chunk function generation; pool_submit call with wrong cast
- `src/codegen/codegen.h` — Iron_Codegen context struct (parallel_counter, lifted_funcs, lambda_counter)
- `src/runtime/iron_runtime.h` — Iron_String type, Iron_pool_submit signature, Iron_global_pool
- `src/runtime/iron_string.c` — iron_string_from_literal, iron_string_from_cstr implementations
- `src/stdlib/iron_math.h` — Full C API: Iron_math_sin/cos/tan/sqrt/pow/floor/ceil/round/lerp, Iron_RNG, Iron_math_random
- `src/stdlib/iron_io.h` — Full C API: Iron_io_read_file/write_file, Iron_Result_String_Error typedef
- `src/stdlib/iron_time.h` — Full C API: Iron_time_now/now_ms/sleep, Iron_Timer
- `src/stdlib/iron_log.h` — Full C API: Iron_log_set_level/debug/info/warn/error, Iron_LogLevel enum
- `src/stdlib/raylib.iron` — Existing .iron wrapper template (extern func pattern)
- `src/cli/build.c` — strstr import detection at line 548; unconditional stdlib linking at lines 303-306; build_src_list function
- `src/analyzer/resolve.c:143` — IRON_NODE_IMPORT_DECL handling (placeholder only)

### Secondary (MEDIUM confidence)
- `.planning/phases/05-codegen-fixes-stdlib-wiring/05-CONTEXT.md` — All implementation decisions verified against source code
- `.planning/STATE.md` — Accumulated decisions from prior phases confirming patterns

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all files directly inspected
- Architecture patterns: HIGH — based on existing code patterns, not speculation
- Pitfalls: HIGH — several are verified bugs in current code (parallel-for cast, empty interp stub)
- Open questions: MEDIUM — identified gaps that require reading one more file each

**Research date:** 2026-03-26
**Valid until:** 2026-04-25 (stable codebase — no external dependencies changing)
