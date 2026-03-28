---
gsd_state_version: 1.0
milestone: v0.0
milestone_name: milestone
status: Roadmap ready, awaiting plan-phase
stopped_at: Completed 12-03-PLAN.md (install pipeline, install.sh, CI, e2e verification)
last_updated: "2026-03-28T02:29:12.792Z"
last_activity: 2026-03-27 — Roadmap created for v0.0.3-alpha (Phases 12-14)
progress:
  total_phases: 3
  completed_phases: 1
  total_plans: 3
  completed_plans: 3
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-27)

**Core value:** Every Iron language feature compiles to correct, working C code that produces a native binary
**Current focus:** v0.0.3-alpha Package Manager — binary split, project workflow, dependency resolution

## Current Position

Phase: 12 (not started)
Plan: —
Status: Roadmap ready, awaiting plan-phase
Last activity: 2026-03-27 — Roadmap created for v0.0.3-alpha (Phases 12-14)

```
[Phase 12] [Phase 13] [Phase 14]
    [ ]         [ ]        [ ]
  0% complete
```

## Performance Metrics

**Velocity (from v0.0.2-alpha):**
- Total plans completed: 16
- Average duration: ~15 min/plan
- Total execution time: ~4 hours

**By Phase (v0.0.2-alpha):**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| Phase 07-ir-foundation | 2 | ~10 min | 5 min |
| Phase 08-ast-to-ir-lowering | 3 | ~69 min | 23 min |
| Phase 09-c-emission-and-cutover | 4 | ~156 min | 39 min |
| Phase 10-test-hardening | 5 | ~15 min | 3 min |
| Phase 11-release-pipeline-versioning | 2 | ~4 min | 2 min |
| Phase 12-binary-split-and-installation P01 | 18 | 3 tasks | 8 files |
| Phase 12-binary-split-and-installation P02 | 12 | 2 tasks | 2 files |
| Phase 12-binary-split-and-installation P03 | 25 | 3 tasks | 5 files |

## Accumulated Context

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- iron/ironc split — iron = package manager, ironc = raw compiler. Cargo/rustc model
- iron CLI in C — same language as compiler, single build system, no extra deps
- Source-based deps — clone from GitHub, compile locally. No registry needed initially
- Cargo-style iron.toml — familiar to Rust developers, proven format for project manifests
- ironc discovered at runtime via sibling-binary detection — not compile-time hardcoded path
- Dependency storage in ~/.iron/cache/ — global per-user, shared across projects
- Source concatenation for multi-file builds — deps + project source -> combined.iron -> ironc
- iron.lock uses lock_version = 1 as first field — parser rejects unknown versions
- Always store 40-char SHAs in iron.lock — never mutable tag names (supply chain safety)
- GitHub tarball download via curl subprocess — faster than full git clone, no libgit2
- toml.c extended in-place for [dependencies] inline tables — no new external TOML libs
- Colored output via ANSI + isatty() + NO_COLOR env var + Windows SetConsoleMode
- [Phase 12-binary-split-and-installation]: get_iron_lib_dir() probes sibling ../lib/ before falling back to IRON_SOURCE_DIR — no behavior change in dev builds
- [Phase 12-binary-split-and-installation]: Static duplication of resolve_self_dir/get_iron_lib_dir in build.c and check.c — shared header factored out later
- [Phase 12-binary-split-and-installation]: iron exits 0 for bare invocation (Cargo-style); ironc exits 1 — iron is user-facing, ironc is developer-facing raw compiler
- [Phase 12-binary-split-and-installation]: iron binary does NOT link iron_compiler — clean process boundary via subprocess invocation (posix_spawnp / CreateProcessA)
- [Phase 12-binary-split-and-installation]: CMake install uses FILES_MATCHING *.c *.h for stdlib — generated C needs stdlib/iron_math.h, stdlib C files need bare iron_math.h
- [Phase 12-binary-split-and-installation]: -I{lib}/stdlib added to clang invocation — stdlib C files at installed prefix need explicit include path for their own headers

### v0.0.2-alpha Accumulated Context (preserved)

- [Phase 07-ir-foundation]: Printer uses temporary Iron_Arena for type_to_string calls — freed before return, no leak
- [Phase 07-ir-foundation]: Verifier collects all errors without early exit — reports all violations in one pass
- [Phase 08-ast-to-ir-lowering]: Iron_CallExpr has no func_decl field — direct calls emit func_ref+func_ptr
- [Phase 08-ast-to-ir-lowering]: Params use alloca+load model with synthetic ValueIds for uniform IDENT resolution
- [Phase 08-ast-to-ir-lowering]: ctx->current_block = NULL after return for dead code suppression
- [Phase 09-c-emission-and-cutover]: mangle_func_name() applies Iron_ prefix at emission time — IR stores names unmangled
- [Phase 09-c-emission-and-cutover]: FUNC_REF + CALL optimization avoids ISO-C-invalid variadic pointer cast
- [Phase 10-test-hardening]: Arena converted to linked-list of non-moving 256KB chunks — fixes use-after-free
- [Phase 10-test-hardening]: Return-value style required for all array algorithms — Iron arrays share pointer on struct copy
- [Phase 11-release-pipeline-versioning]: PROJECT_VERSION from project(VERSION ...) is single source of truth
- [Phase 11-release-pipeline-versioning]: Release triggered on types: [created] — fires on draft and published releases

### v0.0.1-alpha Accumulated Context (preserved)

- stb_ds used throughout for hash maps and dynamic arrays
- Arena allocator is primary allocation strategy in compiler
- Codegen uses fprintf to emit C text directly
- Runtime library is separate CMake target (iron_runtime)
- Integration tests: iron build -> execute binary -> compare stdout
- Cross-platform: pthreads on all platforms (pthreads4w on Windows)

### Pending Todos

None yet.

### Blockers/Concerns

- Phase 3 (DEP-03/04/05): GitHub API rate limits for unauthenticated tag resolution in CI — specify IRON_GIT_TOKEN env var pattern during plan-phase
- Phase 3 (DEP-05): Exact concatenation order for transitive deps and whether iron.lock captures all transitive deps needs to be specified during plan-phase
- All phases: Windows testing coverage for subprocess invocation, path handling, and colored output

## Session Continuity

Last session: 2026-03-28T02:29:12.789Z
Stopped at: Completed 12-03-PLAN.md (install pipeline, install.sh, CI, e2e verification)
Resume file: None
