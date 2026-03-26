# Phase 5: Codegen Fixes + Stdlib Wiring - Context

**Gathered:** 2026-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

String interpolation and parallel-for produce correct output; stdlib modules (math, io, time, log) are callable from Iron source via `import`. This phase closes gaps from the v1.0 milestone audit: GEN-01 (string interpolation codegen), GEN-11 (parallel-for range splitting), STD-01 through STD-04 (stdlib module wiring).

</domain>

<decisions>
## Implementation Decisions

### String Interpolation Codegen
- snprintf-based formatting: `"value is {x}"` emits C code using snprintf with format specifiers per expression type
- `iron_to_cstring()` runtime helper: all types go through a single conversion function — Int, Float, Bool, String handled natively; objects with a `to_string() -> String` method call that method; objects without `to_string()` are a compile error in interpolation
- Full expressions supported inside `{}` — arithmetic, method calls, field access all valid (parser already handles sub-parser for {expr} segments)
- Float formatting uses `%g` (trims trailing zeros): 3.14159 stays 3.14159, 3.0 shows as 3
- Two-pass snprintf allocation: first `snprintf(NULL, 0, ...)` to measure length; if fits in `char[1024]` stack buffer use that, else malloc exact size; then `iron_string_from_literal()` to wrap as Iron_String
- Escaped braces supported: `\{` and `\}` emit literal `{` and `}` (lexer already detects interpolation via unescaped `{` only)

### Stdlib Import Wiring
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

### Parallel-For Codegen
- Context struct for captured variables: codegen detects outer variable references in the for body, generates a struct with pointers to captured vars, passes as `void* ctx_arg` (slot already exists in chunk function signature)
- Implicit barrier: `parallel for` always blocks until all chunks complete — code after the block can safely read results
- Array indexing allowed: `array[i] = compute(i)` is safe because each iteration writes to a distinct index — concurrency checker permits this alongside its existing "no mutation of outer non-mutex vars" rule
- Panic propagation: first panic sets a shared error flag; other chunks check flag at iteration start and bail; after barrier, panic is re-raised on the main thread (matches spawn/await panic model)

### Integration Testing
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

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Language Specification
- `docs/language_definition.md` -- String interpolation syntax, parallel-for semantics, import mechanism, object method rules

### Codegen (MUST READ -- these are the files being fixed)
- `src/codegen/gen_exprs.c` -- IRON_NODE_INTERP_STRING stub at line ~291 (currently emits ""), expression emission patterns
- `src/codegen/gen_stmts.c` -- Parallel-for codegen at line ~234, chunk function generation, pool_submit emission
- `src/codegen/codegen.c` -- Main codegen orchestration, includes buffer, lifted_funcs output section
- `src/codegen/codegen.h` -- Iron_Codegen context struct (parallel_counter, lifted_funcs, lambda_counter)

### Runtime (interpolation helper lives here)
- `src/runtime/iron_runtime.h` -- Iron_String type, Iron_Error type, runtime init/shutdown
- `src/runtime/iron_string.c` -- iron_string_from_literal, SSO implementation
- `src/runtime/iron_threads.c` -- Iron_pool_submit, Iron_pool_barrier, Iron_global_pool

### Stdlib (existing C implementations to wire)
- `src/stdlib/iron_math.h` -- Iron_math_sin, Iron_math_cos, IRON_PI, Iron_RNG etc.
- `src/stdlib/iron_io.h` -- Iron_io_read_file (returns Iron_Result_String_Error), Iron_io_write_file etc.
- `src/stdlib/iron_time.h` -- Iron_time_now, Iron_time_sleep, Iron_Timer
- `src/stdlib/iron_log.h` -- Iron_log_info/warn/error/debug, Iron_log_set_level, Iron_LogLevel enum
- `src/stdlib/raylib.iron` -- Existing .iron wrapper pattern to follow for new stdlib wrappers

### Analyzer (auto-static detection + import resolution)
- `src/analyzer/resolve.c` -- IRON_NODE_IMPORT_DECL handling (currently placeholder), symbol registration
- `src/analyzer/typecheck.c` -- Type checking (needs auto-static method validation)
- `src/analyzer/concurrency.c` -- Concurrency checker (parallel-for body rules, outer var access)

### Build Pipeline
- `src/cli/build.c` -- strstr-based import detection, stdlib .c file linking, raylib.iron prepend pattern

### Prior Phase Context
- `.planning/phases/03-runtime-stdlib-and-cli/03-CONTEXT.md` -- Threading model, stdlib API design, error handling pattern
- `.planning/phases/04-comptime-game-dev-and-cross-platform/04-CONTEXT.md` -- extern func pattern, raylib.iron wrapper approach, import raylib wiring

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `src/codegen/gen_exprs.c`: Expression emission patterns — interpolation codegen extends the existing IRON_NODE_INTERP_STRING case
- `src/codegen/gen_stmts.c`: Parallel-for chunk generation already implemented — needs capture struct addition and correctness fixes
- `src/stdlib/raylib.iron`: Existing .iron wrapper file — template for math.iron, io.iron, time.iron, log.iron
- `src/runtime/iron_string.c`: iron_string_from_literal — used to wrap snprintf output as Iron_String
- `src/cli/build.c`: strstr import detection + .iron file prepend — extend for stdlib modules

### Established Patterns
- Iron_ prefix on all C types and functions
- .iron wrapper files with extern func declarations for FFI (raylib.iron pattern)
- strstr-based import detection in build.c
- Lifted functions buffer (ctx->lifted_funcs) for chunk functions
- Anonymous result structs for multi-return (Iron_Result_String_Error pattern)
- Integration tests: .iron file + .expected file, build and run, compare stdout

### Integration Points
- Codegen IRON_NODE_INTERP_STRING: currently emits "" — needs snprintf implementation
- Codegen parallel-for: chunk function needs ctx_arg unpacking for captured variables
- Resolver IRON_NODE_IMPORT_DECL: needs to register module object symbols (Math, IO, Time, Log)
- Typecheck: needs auto-static detection (methods without self/instance-var access)
- build.c: needs strstr detection for import math/io/time/log + .iron prepend

</code_context>

<specifics>
## Specific Ideas

- Auto-static is a GENERAL language feature — not stdlib-specific. Any object method that doesn't use `self` or reference instance `var` fields should be callable as `Object.method()` without an instance
- The success criteria literally say: `import math` followed by `val s = sin(1.0)` — but per discussion, the API is `Math.sin(1.0)` (module-qualified). Update tests accordingly
- `iron_to_cstring()` should support objects with `to_string() -> String` method — makes interpolation work for user-defined types that implement the pattern
- Parallel-for with `array[i] = i` is THE canonical use case from the success criteria — must work correctly with capture struct passing the array pointer

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 05-codegen-fixes-stdlib-wiring*
*Context gathered: 2026-03-26*
