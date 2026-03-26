---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: milestone
status: executing
stopped_at: Completed 03-06-PLAN.md
last_updated: "2026-03-26T13:14:42.929Z"
last_activity: "2026-03-25 — Completed plan 03-04: codegen runtime integration — generated C now includes iron_runtime.h, parallel-for uses dynamic thread count, builtins len/min/max/clamp/abs/assert registered in resolver, 5 new integration tests, all 17 tests passing"
progress:
  total_phases: 4
  completed_phases: 2
  total_plans: 20
  completed_plans: 18
  percent: 80
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-25)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** Phase 2 — Semantics and Codegen

## Current Position

Phase: 3 of 4 (Runtime, Stdlib, and CLI)
Plan: 4 of 8 in current phase (plans 01-04 complete)
Status: In progress
Last activity: 2026-03-25 — Completed plan 03-04: codegen runtime integration — generated C now includes iron_runtime.h, parallel-for uses dynamic thread count, builtins len/min/max/clamp/abs/assert registered in resolver, 5 new integration tests, all 17 tests passing

Progress: [████████░░] 80%

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

### Pending Todos

None yet.

### Blockers/Concerns

- [Research] Escape analysis precision contract (intra-procedural boundary) must be decided before Phase 2 semantic planning begins
- [Research] `rc` cycle detection strategy (`weak T` or debug-mode only) must be decided before Phase 3 runtime planning begins
- [Research] Generics monomorphization deduplication mechanism must be decided before Phase 2 codegen planning begins
- [Research] Windows threading abstraction (C11 threads vs tinycthread vs Win32) needs a spike before Phase 3 runtime planning begins
- [Research] Raylib binding strategy (hand-written vs header-generated) needs a decision before Phase 4 planning begins

## Session Continuity

Last session: 2026-03-26T13:14:42.926Z
Stopped at: Completed 03-06-PLAN.md
Resume file: None
