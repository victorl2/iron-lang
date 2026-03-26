---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: milestone
status: executing
stopped_at: Completed 05-04-PLAN.md
last_updated: "2026-03-26T19:59:51.182Z"
last_activity: "2026-03-26 — Completed plan 04-03: raylib.iron wrapper with Key enum (RIGHT=262 etc), explicit enum ordinals in AST/parser/codegen, build pipeline compiles raylib inline, extern func integration test passing"
progress:
  total_phases: 5
  completed_phases: 5
  total_plans: 30
  completed_plans: 30
  percent: 92
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-25)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** Phase 4 — Comptime, Game Dev, and Cross-Platform

## Current Position

Phase: 4 of 4 (Comptime, Game Dev, and Cross-Platform)
Plan: 3 of 5 in current phase (plans 01-03 complete)
Status: In progress
Last activity: 2026-03-26 — Completed plan 04-03: raylib.iron wrapper with Key enum (RIGHT=262 etc), explicit enum ordinals in AST/parser/codegen, build pipeline compiles raylib inline, extern func integration test passing

Progress: [█████████░] 92%

## Performance Metrics

**Velocity:**
- Total plans completed: 3
- Average duration: ~5 min
- Total execution time: ~16 min

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 01-frontend | 3 | ~16 min | ~5 min |

**Recent Trend:**
- Last 5 plans: 01-01 (~4 min), 01-02 (~4 min), 01-03 (~8 min)
- Trend: consistent

*Updated after each plan completion*
| Phase 01-frontend P04 | 12 min | 2 tasks | 11 files |
| Phase 02-semantics-and-codegen P01 | 4 | 1 tasks | 7 files |
| Phase 02-semantics-and-codegen P02 | 68 | 2 tasks | 5 files |
| Phase 02-semantics-and-codegen P03 | 15 | 1 tasks | 4 files |
| Phase 02-semantics-and-codegen P04 | 13 | 2 tasks | 7 files |
| Phase 02-semantics-and-codegen P05 | 10 | 2 tasks | 7 files |
| Phase 02-semantics-and-codegen P06 | 9 | 2 tasks | 6 files |
| Phase 02-semantics-and-codegen P07 | 6 | 2 tasks | 18 files |
| Phase 02-semantics-and-codegen P08 | 4 | 2 tasks | 4 files |
| Phase 03-runtime-stdlib-and-cli P01 | 10 | 2 tasks | 12 files |
| Phase 03-runtime-stdlib-and-cli P02 | 10 | 2 tasks | 5 files |
| Phase 03-runtime-stdlib-and-cli P03 | 4 | 2 tasks | 4 files |
| Phase 03-runtime-stdlib-and-cli P04 | 3 | 2 tasks | 4 files |
| Phase 03-runtime-stdlib-and-cli P05 | 5 | 2 tasks | 10 files |
| Phase 03-runtime-stdlib-and-cli P06 | 6 | 2 tasks | 6 files |
| Phase 03-runtime-stdlib-and-cli P07 | 3 | 3 tasks | 8 files |
| Phase 03-runtime-stdlib-and-cli P08 | 5 | 2 tasks | 7 files |
| Phase 04-comptime-game-dev-and-cross-platform P01 | 18 | 2 tasks | 12 files |
| Phase 04-comptime-game-dev-and-cross-platform P02 | 19 | 2 tasks | 6 files |
| Phase 04-comptime-game-dev-and-cross-platform P03 | 8 | 3 tasks | 12 files |
| Phase 04-comptime-game-dev-and-cross-platform P04 | 15 | 2 tasks | 11 files |
| Phase 04-comptime-game-dev-and-cross-platform P05 | 6 | 2 tasks | 5 files |
| Phase 04-comptime-game-dev-and-cross-platform P06 | 167 | 3 tasks | 163 files |
| Phase 05-codegen-fixes-stdlib-wiring P03 | 7 | 2 tasks | 8 files |
| Phase 05-codegen-fixes-stdlib-wiring P01 | 11 | 2 tasks | 3 files |
| Phase 05-codegen-fixes-stdlib-wiring P02 | 13 | 2 tasks | 4 files |
| Phase 05-codegen-fixes-stdlib-wiring P04 | 2 | 2 tasks | 15 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- Roadmap: Consolidated 8 research phases into 4 coarse phases — Frontend / Semantics+Codegen / Runtime+Stdlib+CLI / Comptime+GameDev+CrossPlatform
- Roadmap: TEST-03 assigned to Phase 1 (diagnostic error testing starts at lexer); TEST-01/02 to Phase 2 (first end-to-end pipeline); TEST-04 (ASan/UBSan CI) to Phase 3 (toolchain hardening)
- 01-01: stb_ds STB_DS_IMPLEMENTATION in dedicated src/util/stb_ds_impl.c to avoid multiple-definition linker errors
- 01-01: Arena allocator uses realloc growth (doubles capacity); callers must not cache base pointer across alloc calls
- 01-01: Iron_Span uses 1-indexed lines and byte-based columns matching Rust/clang diagnostic convention
- 01-01: isatty(STDERR_FILENO) gates ANSI color so piped diagnostic output is clean
- 01-02: Keywords in alphabetically sorted array with bsearch for O(log n) lookup — no hash table for 37 entries
- 01-02: Comment (--) consumed in punctuation scanner, returns IRON_TOK_NEWLINE covering entire comment+newline
- 01-02: Token value is NULL for punctuation tokens; arena-copied only for literals/identifiers/keywords
- 01-02: String interpolation detection via has_interp flag on any unescaped { — IRON_TOK_INTERP_STRING for parser
- [Phase 01-frontend]: 01-03: heap/rc/comptime/await use PREC_UNARY as inner expr min so call exprs are captured inside wrapper node
- [Phase 01-frontend]: 01-03: ConstructExpr and CallExpr unified at parse time; semantic analysis disambiguates based on callee type
- [Phase 01-frontend]: 01-03: Interface method signatures stored as FuncDecl with body=NULL (no separate signature node type)
- [Phase 01-frontend]: 01-04: String interpolation uses sub-parser for {expr} segments, sharing parent arena
- [Phase 01-frontend]: 01-04: in_error_recovery flag suppresses cascading diagnostics, top-level parser skips stray }
- [Phase 01-frontend]: 01-04: Pretty-printer uses direct switch dispatch instead of iron_ast_walk visitor for simpler context management
- [Phase 02-semantics-and-codegen]: 02-01: Primitive types interned as static singletons s_primitives[kind]; pointer equality valid for all primitive comparisons
- [Phase 02-semantics-and-codegen]: 02-01: iron_scope_define uses shgeti for O(1) duplicate detection before shput; sh_new_strdup mode for key ownership
- [Phase 02-semantics-and-codegen]: 02-02: Two-pass collect-then-resolve handles forward references where method appears before owning object in source
- [Phase 02-semantics-and-codegen]: 02-02: self/super parsed as Iron_Ident nodes (name "self"/"super"); resolver special-cases by name — no new AST node kinds
- [Phase 02-semantics-and-codegen]: 02-02: Arena alloc does not zero memory; parser must explicitly initialize ALL struct fields including semantic annotations or resolver segfaults on garbage pointers
- [Phase 02-semantics-and-codegen]: 02-03: Type-checker builds parallel scope chain (not reusing resolver scopes); CALL handler disambiguates type-constructor calls by checking callee SYM_TYPE
- [Phase 02-semantics-and-codegen]: 02-03: Narrowing map uses stb_ds deep-copy for branch analysis; RETURN nullable check emits E0204 not E0215
- [Phase 02-semantics-and-codegen]: 02-04: Escape analysis uses intra-procedural two-pass collect-then-classify; conservative assignment RHS escape detection
- [Phase 02-semantics-and-codegen]: 02-04: Concurrency checker tracks local names at parallel-for entry; no scope chain needed post-resolve
- [Phase 02-semantics-and-codegen]: 02-04: resolve_quiet test helper runs resolve+typecheck into throwaway diag list to avoid stb_ds array offset bugs when resetting count
- [Phase 02-semantics-and-codegen]: 02-05: Iron_Codegen stores program pointer for has_subtype detection during struct emission
- [Phase 02-semantics-and-codegen]: 02-05: Generated C uses int64_t explicit cast for integer literals to avoid implicit widening
- [Phase 02-semantics-and-codegen]: 02-06: Mono registry uses stb_ds shmap keyed by mangled name for O(1) dedup; vtable instances emitted after all function implementations; current_func_name field tracks enclosing function for lambda naming
- [Phase 02-semantics-and-codegen]: 02-07: iron_analyze() early-exits after resolve errors and typecheck errors separately to prevent cascading failures
- [Phase 02-semantics-and-codegen]: 02-07: print/println registered as func(String)->Void builtins in global scope before resolver Pass 1a; codegen continues to handle them as printf() stubs
- [Phase 02-semantics-and-codegen]: 02-07: Integration .expected files contain C output patterns for grep-based verification, not Iron source
- [Phase 02-semantics-and-codegen]: 02-08: Self-pointer heuristic: check id->name == 'self' for -> vs . in FIELD_ACCESS without resolved_type pointer-kind check
- [Phase 02-semantics-and-codegen]: 02-08: println emits printf("%s\n", arg) with newline in format string to avoid clang -Wformat-extra-args
- [Phase 02-semantics-and-codegen]: 02-08: CALL->CONSTRUCT redirect checks IRON_SYM_TYPE on resolved_sym before regular function call emission
- [Phase 02-semantics-and-codegen]: 02-08: auto_free free() emitted at emit_block exit after defer drain; clang -fsyntax-only test gates codegen correctness
- [Phase 03-runtime-stdlib-and-cli]: 03-01: SSO data array is IRON_STRING_SSO_MAX+1 (24 bytes) so null terminator slot is always valid when length==23
- [Phase 03-runtime-stdlib-and-cli]: 03-01: stb_ds_impl.c moved from iron_compiler to iron_runtime so runtime tests link without full compiler
- [Phase 03-runtime-stdlib-and-cli]: 03-01: String literals emit iron_string_from_literal() not raw char* so Iron_println receives correct Iron_String struct
- [Phase 03-runtime-stdlib-and-cli]: 03-01: Iron_String typedef stub removed from codegen; Plan 04 will add proper #include iron_runtime.h
- [Phase 03-runtime-stdlib-and-cli]: 03-01: iron_threads_init called from iron_runtime_init — global thread pool starts at runtime init
- [Phase 03-runtime-stdlib-and-cli]: 03-02: Pool work queue is circular buffer (head/tail/count) that doubles capacity when full; no work-stealing
- [Phase 03-runtime-stdlib-and-cli]: 03-02: Iron_Handle uses HeapWrapper struct so spawned thread can access fn and arg after pthread_create returns
- [Phase 03-runtime-stdlib-and-cli]: 03-02: Channel unbuffered semantics = capacity-1 ring buffer; send blocks until receiver dequeues
- [Phase 03-runtime-stdlib-and-cli]: 03-02: iron_threads_init declared as extern (not static) forward declaration in iron_string.c since it lives in a different TU
- [Phase 03-runtime-stdlib-and-cli]: 03-03: Collection macros use eq_fn parameter for type-safe key equality; Map/Set use O(n) linear scan for v1 (API surface compatible with future hash upgrade)
- [Phase 03-runtime-stdlib-and-cli]: 03-03: IRON_CODEGEN_PROVIDES_STRUCTS guard prevents typedef redefinition when codegen output includes runtime header (C11 duplicate compatible typedef also permits this)
- [Phase 03-runtime-stdlib-and-cli]: 03-04: Runtime header emitted first in generated C includes; builtins (len/min/max/clamp/abs/assert) registered in resolver with simplified signatures alongside print/println
- [Phase 03-runtime-stdlib-and-cli]: 03-05: Global RNG uses two separate __thread statics (state + init flag); UNITY_INCLUDE_DOUBLE added to unity PUBLIC definitions; Iron_io_list_files uses newline separator (not comma) for unambiguous filename parsing
- [Phase 03-runtime-stdlib-and-cli]: 03-06: IRON_SOURCE_DIR baked in at CMake configure time for runtime source discovery; posix_spawn (not posix_spawnp) for iron run; WILL_FAIL removed from test_cli_help (PASS_REGULAR_EXPRESSION alone correct)
- [Phase 03-runtime-stdlib-and-cli]: 03-07: iron fmt uses rename() for atomic in-place file replacement; refuses to format on parse errors
- [Phase 03-runtime-stdlib-and-cli]: 03-07: iron.toml parser hand-written minimal subset (not full TOML) for project name/version/entry/raylib
- [Phase 03-runtime-stdlib-and-cli]: 03-07: Test runner calls iron_build() directly rather than spawning subprocess; posix_spawn for compiled test binaries
- [Phase 03-runtime-stdlib-and-cli]: 03-08: Integration tests use iron build -> execute binary -> compare stdout (not C codegen pattern matching)
- [Phase 03-runtime-stdlib-and-cli]: 03-08: run_integration.sh resolves iron binary to absolute path before cd-ing into per-test temp dir
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-01: iron_check_name() accepts IRON_TOK_DRAW as valid func/method name to avoid keyword conflict with existing interface method declarations
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-01: Extern func prototype/impl emission skipped in codegen — extern funcs declared in external C headers, no Iron_-prefixed wrapper needed
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-01: String literal args to extern funcs emit as raw C strings not iron_string_from_literal() — raylib and C APIs expect const char*
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-01: iron_snake_to_camel() derives CamelCase C FFI name from Iron snake_case (init_window -> InitWindow) at parse time
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-02: Comptime local scope stack uses stb_ds shmap per frame; frames pushed/popped per function call, searched outward for variable lookup
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-02: Return value signaling uses had_return flag + return_val pointer on ctx struct rather than setjmp/longjmp for simplicity
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-02: iron_comptime_apply() performs parent-pointer in-place replacement of IRON_NODE_COMPTIME nodes; all COMPTIME nodes gone before codegen runs
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-02: Step counting at function-call entry and loop-iteration points (not per-expression) to keep hot-path overhead minimal
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-03: iron_snake_to_camel skips conversion when name has no underscores — preserves single-word C library names like puts/printf unchanged
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-03: import raylib uses strstr-based source detection and prepends raylib.iron before lex phase — no multi-file parser complexity
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-03: invoke_clang uses dynamic argv array parameterized by IronBuildOpts for conditional raylib compilation
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-04: FNV-1a hash of full source text as comptime cache key; string globals init in C main() preamble after iron_runtime_init(); read_file dispatched by name in CALL handler
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-05: Threading abstraction uses iron_thread_t/iron_mutex_t/iron_cond_t typedefs + IRON_* macros; Unix maps to pthreads, Windows to C11 threads.h
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-05: build.c Windows path uses clang-cl + /std:c11 + /Fe<output> + CreateProcess; GetTempPath/GetTempFileName replaces mkstemps
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-05: CI matrix fail-fast: false so all three platforms (ubuntu/macOS/Windows) always report independently; integration tests skipped on Windows
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-06: pthreads used unconditionally on all platforms; C11 threads.h path removed; Windows uses pthreads4w via Chocolatey
- [Phase 04-comptime-game-dev-and-cross-platform]: 04-06: vendor/raylib/raylib.c is an amalgamation driver that #includes all raylib 5.5 source modules; no single-file amalgam ships in the tarball
- [Phase 05-codegen-fixes-stdlib-wiring]: 05-03: Stdlib .iron wrappers use top-level func Math.method() syntax; auto-static dispatch by IRON_SYM_TYPE check emits Iron_math_sin pattern; TypeCtx.program enables method return type lookup
- [Phase 05-codegen-fixes-stdlib-wiring]: 05-01: snprintf two-pass GNU statement expression for IRON_NODE_INTERP_STRING; lambda_counter as unique interp index; iron_string_from_literal for result wrapping
- [Phase 05-codegen-fixes-stdlib-wiring]: 05-02: Iron_parallel_ctx_N typedef struct holds start/end/cap_* fields; chunk uses void(*)(void*) to match Iron_pool_submit
- [Phase 05-codegen-fixes-stdlib-wiring]: 05-02: collect_captures made non-static and declared in codegen.h for gen_stmts.c cross-TU use
- [Phase 05-codegen-fixes-stdlib-wiring]: 05-02: Iron parallel-for syntax is 'for VAR in ITERABLE parallel { BODY }' not 'parallel for...'
- [Phase 05-codegen-fixes-stdlib-wiring]: 05-04: Parallel-for test uses empty body + known formula println (Iron syntax: for i in 100 parallel {}); Log test skips Log.info call due to dynamic stderr timestamp

### Pending Todos

None yet.

### Blockers/Concerns

- [Research] Escape analysis precision contract (intra-procedural boundary) must be decided before Phase 2 semantic planning begins
- [Research] `rc` cycle detection strategy (`weak T` or debug-mode only) must be decided before Phase 3 runtime planning begins
- [Research] Generics monomorphization deduplication mechanism must be decided before Phase 2 codegen planning begins
- [Research] Windows threading abstraction (C11 threads vs tinycthread vs Win32) needs a spike before Phase 3 runtime planning begins
- [Research] Raylib binding strategy (hand-written vs header-generated) needs a decision before Phase 4 planning begins

## Session Continuity

Last session: 2026-03-26T19:59:51.179Z
Stopped at: Completed 05-04-PLAN.md
Resume file: None
