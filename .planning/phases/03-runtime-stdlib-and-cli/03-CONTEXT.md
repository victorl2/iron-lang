# Phase 3: Runtime, Stdlib, and CLI - Context

**Gathered:** 2025-03-26
**Status:** Ready for planning

<domain>
## Phase Boundary

Iron programs can be built, run, checked, formatted, and tested via the `iron` CLI, backed by a complete runtime and standard library. This phase delivers: runtime library (strings, collections, rc, threading), standard library modules (math, io, time, log), CLI toolchain (build, run, check, fmt, test), CI setup, and ASan/UBSan hardening. Comptime evaluation, raylib bindings, and Windows parity are separate phases (Phase 4).

</domain>

<decisions>
## Implementation Decisions

### String and Collection Design
- Full SSO + interning: small strings (<=23 bytes) stored inline, larger strings heap-allocated with global intern table for deduplication
- `weak T` keyword added to the language for rc cycle breaking — like Swift's weak/unowned references. Game entity-scene graphs will hit cycles without this.
- Method syntax for collections: `list.push(item)`, `map.get(key)`, `set.contains(val)` — matches Iron's OOP method style
- Generic collections from the start: List[T], Map[K,V], Set[T] monomorphized per Phase 2 decisions — runtime provides the template implementations

### Threading and Concurrency
- C11 threads only (`<threads.h>`) — clang is the sole target compiler, leverage clang features (sanitizers, thread safety annotations, etc.)
- Default pool size: CPU count - 1 (leave one core for main/render thread)
- Channels unbuffered by default (send blocks until recv) — explicit buffering via `channel[T](size)`
- Named thread pools: `val compute = pool("compute", 4)` as in the language spec — games need separate pools for physics, AI, rendering
- Mutex[T] + raw Lock/CondVar primitives for advanced users
- Panic propagates on await: panic stored in Future, re-raised when parent calls await — like Rust's JoinHandle
- Parallel-for always auto-splits (range / pool_size) — no configurable chunk size in v1
- await_all helper: `await_all([task1, task2])` waits for all futures to complete

### CLI Command Behavior
- Build directory: .iron-build/ kept when `--debug-build` flag provided, otherwise temp dir cleaned up
- Timestamp-based build cache: skip recompilation if source hasn't changed since last build
- Test discovery: `test_` prefix convention — any .iron file starting with `test_` is a test file
- `iron fmt` is in-place by default (like gofmt)
- `iron check` = parse + analyze only — no codegen, no clang invocation (fast IDE-like feedback)
- Binary name matches source: `hello.iron` → `hello` (or `hello.exe` on Windows)
- `iron run` passes args after `--`: `iron run hello.iron -- arg1 arg2`
- Basic `iron.toml` project file for multi-file projects:
  ```toml
  [project]
  name = "my_game"
  version = "0.1.0"
  entry = "src/main.iron"

  [dependencies]
  raylib = true
  ```
- Clang-only as target C compiler — no gcc support needed (simplifies build pipeline, enables clang-specific features)

### Standard Library
- File I/O uses multiple return values for errors: `func read_file(path: String) -> String, Error` — Go-style error handling. Codegen emits anonymous result struct `Iron_Result_String_Error { Iron_String v0; Iron_Error v1; }`.
- Log format is timestamped: `[2025-03-25 12:00:00] [INFO] message` — useful for game dev debugging
- RNG provides both global convenience functions (`random()`, `random_int(min, max)`) and explicit RNG objects (`val rng = RNG(seed); rng.next_int(min, max)`) for deterministic replays
- math module wraps `<math.h>` with Iron types: `sin`, `cos`, `tan`, `sqrt`, `pow`, `lerp`, `floor`, `ceil`, `round`, `PI`, `TAU`, `E`
- io module: `read_file`, `read_bytes`, `write_file`, `write_bytes`, `file_exists`, `list_files`, `create_dir`, `delete_file` — all with Error return
- time module: `now`, `now_ms`, `sleep`, `since`, Timer object
- log module: `info`, `warn`, `error`, `debug` with `set_level` — output to stderr

### CI Setup
- GitHub Actions set up in this phase (deferred from Phase 1)
- CMake build + full test suite + ASan/UBSan on macOS and Linux
- TEST-04 requirement fulfilled here

### Carried Forward from Phase 1 & 2
- Iron_ prefix on all runtime types/functions
- C main() calls Iron_main() — runtime init/shutdown wraps this call
- Rust-style diagnostics (3-line window, E-codes, ANSI color via isatty)
- CMake + Ninja build system, Unity test framework
- Codegen produces single .c output file
- print/println currently emit printf stubs — runtime provides real Iron_String-aware implementations
- Generated C compiles with `clang -std=c11 -Wall -Werror`

### Claude's Discretion
- SSO threshold (23 bytes is suggested, exact value flexible)
- Intern table implementation details (hash map size, eviction policy)
- Thread pool work-stealing vs FIFO queue
- Build cache storage format
- iron.toml parser implementation (hand-written vs library)
- Exact CLI argument parsing approach
- Error type design details
- Multiple return value codegen implementation (anonymous struct naming)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Language Specification
- `docs/language_definition.md` — String semantics, collection types, concurrency primitives (spawn, await, channel, mutex, parallel for), memory keywords (heap, free, leak, defer, rc)
- `docs/implementation_plan.md` — Phase 5 (runtime components), Phase 6 (stdlib modules), Phase 7 (CLI commands and build pipeline)

### Research
- `.planning/research/STACK.md` — Recommended stack, build system, ASan/UBSan strategy
- `.planning/research/ARCHITECTURE.md` — Compiler pipeline, runtime integration points
- `.planning/research/PITFALLS.md` — Windows pthread issues (moot now — C11 threads + clang only), rc cycle detection, string UTF-8 edge cases

### Phase 1 & 2 Code (MUST READ)
- `src/codegen/codegen.h` — Iron_Codegen context, C emission API, main() wrapper pattern
- `src/codegen/gen_types.c` — Type mapping (Iron → C), monomorphization registry, Optional struct generation
- `src/codegen/gen_stmts.c` — Defer drain, spawn/parallel-for codegen stubs that runtime must implement
- `src/codegen/gen_exprs.c` — print/println emit printf — runtime must replace these
- `src/analyzer/types.h` — Iron_Type system (26 kinds) that runtime types must align with
- `src/util/arena.h` — Arena allocator pattern used throughout
- `src/util/strbuf.h` — String builder used for C emission
- `CMakeLists.txt` — Build configuration to extend with runtime/stdlib/cli targets

### Project Context
- `.planning/PROJECT.md` — Core value, constraints (clang preferred, cross-platform, Rust-style diagnostics)
- `.planning/REQUIREMENTS.md` — RT-01..08, STD-01..04, CLI-01..08, TEST-04 mapped to this phase
- `.planning/phases/02-semantics-and-codegen/02-CONTEXT.md` — Codegen naming (Iron_ prefix everywhere), main() wrapping, monomorphization

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `src/util/arena.h/c`: Arena allocator — runtime should use same pattern for internal allocations
- `src/util/strbuf.h/c`: String builder — useful for string formatting, log output
- `src/vendor/stb_ds.h`: Hash map — reusable for Map[K,V] runtime implementation and intern table
- `src/diagnostics/diagnostics.h/c`: Diagnostic system — CLI error output uses this
- `src/parser/printer.h/c`: AST pretty-printer — `iron fmt` can reuse this directly

### Established Patterns
- Iron_ prefix on all types and functions
- Arena allocation for compiler data structures (runtime may use different allocation for user data)
- E0001-style error codes with Iron_Span source locations
- Unity test framework with CMake FetchContent
- ASan/UBSan enabled in debug builds
- Single .c output file from codegen

### Integration Points
- Codegen spawn/parallel-for stubs reference runtime functions: `Iron_pool_submit`, `Iron_pool_barrier`, `Iron_Channel_send/recv`
- print/println codegen emits `printf()` — runtime must provide Iron_print/Iron_println that handle Iron_String
- main() wrapper calls `Iron_main()` — needs `iron_runtime_init()` / `iron_runtime_shutdown()` around it
- Collections codegen expects monomorphized structs (Iron_List_Int with items/count/capacity fields)
- The `iron` CLI wraps the full pipeline: lex → parse → analyze → codegen → clang invocation

</code_context>

<specifics>
## Specific Ideas

- Clang is the ONLY target C compiler — leverage clang-specific features: sanitizers, thread safety annotations (`__attribute__((guarded_by))`, `__attribute__((lockable))`), `_Static_assert`, etc.
- Multiple return values for error handling (Go-style): `func read_file() -> String, Error` — this needs codegen support for anonymous result structs
- The `iron fmt` command can directly reuse the existing AST pretty-printer from Phase 1 (`src/parser/printer.c`)
- iron.toml enables multi-file projects and raylib dependency declaration (Phase 4 will use this)

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 03-runtime-stdlib-and-cli*
*Context gathered: 2025-03-26*
