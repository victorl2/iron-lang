---
phase: 67-correctness-fixes-+-crash-canaries
plan: 05
subsystem: correctness
tags: [fix-02, arena, oom, iron_oom_abort, analyzer, comptime, walkthrough]

# Dependency graph
requires:
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: iron_oom_abort helper (67-02) and diagnostics.h wiring
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: 67-03 integer-safety baseline (357 integration pass count)
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: 67-04 parser/lexer walkthrough (established idiom conventions)
provides:
  - Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in src/analyzer/**/*.c guarded with iron_oom_abort on NULL
  - Every iron_arena_alloc / iron_arena_strdup call-site in src/comptime/comptime.c guarded with iron_oom_abort on NULL
  - Location literal at every guard site ("<file>.c:<function>" plus optional sub-site disambiguator) so any OOM abort stderr pinpoints the exact call-site
  - Analyzer + comptime subsystem arena contract upgraded from "silent UB on malloc fail" to "explicit fail-fast with bisectable location"
affects: [67-06 hir + lir + runtime walkthrough, 67-07 cross-arena walkthrough, 67-08 crash canaries]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "iron_oom_abort idiom A: ARENA_ALLOC / iron_arena_alloc / iron_arena_strdup → if (!var) iron_oom_abort(\"file.c:function [disambiguator]\") → dereference"
    - "emit_error / emit_warning inline iron_arena_strdup hoisting: extract inline diag_emit strdup args into named locals, guard with iron_oom_abort, then pass named locals into the diag_emit call"
    - "types.c constructor fallback replacement: every iron_type_make_* `if (!t) return NULL;` bailout replaced with `if (!t) iron_oom_abort(\"types.c:<fn>\");` since callers already assume non-NULL"

key-files:
  created: []
  modified:
    - src/analyzer/typecheck.c (36 sites, 37 guards — 37 = 36 new + 1 pre-existing from 67-03's covered[] dynamic-sizing fix; inline strdups hoisted in emit_error/emit_warning/MATCH note)
    - src/analyzer/resolve.c (8 sites, 8 guards — emit_undefined + PATTERN + ENUM_CONSTRUCT rewrites)
    - src/analyzer/types.c (18 sites, 18 guards — every iron_type_make_* constructor + iron_type_to_string intermediate buffers + tuple_build_mangled_name)
    - src/analyzer/capture.c (6 sites, 6 guards — find_captures, find_spawn_captures, find_pfor_captures each with Iron_CaptureEntry array + name strdup)
    - src/analyzer/scope.c (2 sites, 2 guards — iron_scope_create, iron_symbol_create)
    - src/analyzer/escape.c (1 site, 1 guard — emit_err msg)
    - src/analyzer/concurrency.c (2 sites, 2 guards — emit_err + emit_warn msgs)
    - src/analyzer/init_check.c (1 site, 1 guard — emit_uninit_error msg)
    - src/analyzer/web_await_check.c (1 site, 1 guard — emit_await_error msg)
    - src/analyzer/web_top_level_loader_check.c (1 site, 1 guard — emit_loader_error msg)
    - src/comptime/comptime.c (18 sites, 18 guards — cval_alloc, comptime_cache_read, build_call_trace, iron_comptime_eval_expr read_file/ARRAY_LIT/CONSTRUCT, iron_comptime_val_to_ast IntLit/FloatLit/BoolLit/StringLit/ArrayLit)

key-decisions:
  - "Guard-by-default idiom A applied to every analyzer + comptime arena call-site (95 total guards for 94 sites; the +1 is the pre-existing 67-03 covered[] guard). Zero SAFETY annotations applied — every site is on a user-reachable compilation path with no compile-time bound"
  - "types.c iron_type_make_* constructors had pre-existing `if (!t) return NULL;` fallbacks that callers already assumed non-NULL. Replaced with iron_oom_abort so the caller-side NULL-check can be removed in a follow-up, and so OOM fails loudly with a per-constructor location literal"
  - "Inline iron_arena_strdup calls embedded as iron_diag_emit / emit_error arguments (seen in typecheck.c emit_error/emit_warning, resolve.c 3x sites, escape.c, concurrency.c 2x, init_check.c, both web_* files) must be hoisted into named locals so the NULL check can happen before the diag_emit call. Preserves the diagnostic path while fail-fast on OOM"
  - "Phase 66 IRON_NODE_ASSERT_KIND lines untouched — the walkthrough is purely additive. 67-03 integer-overflow guards in comptime.c (rows 14+15) still intact post-walkthrough: grep `__builtin_add_overflow` / `INT64_MIN` returns 12 hits unchanged"
  - "Sub-site disambiguator convention: functions with multiple allocations use distinct location literals (e.g. `typecheck.c:resolve_type_annotation tuple_elems`, `typecheck.c:resolve_type_annotation func_params`, `typecheck.c:resolve_type_annotation type_args`, etc.). An OOM stderr grep resolves to the exact call-site without ambiguity"

patterns-established:
  - "Analyzer + comptime FIX-02 idiom: every arena alloc produces a function-qualified location literal so stderr OOM aborts bisect cleanly"
  - "Grep-auditable coverage: for each of the 11 files, guard count ≥ site count (deterministic review via `grep -c`)"
  - "diag_emit inline-strdup hoisting pattern: the analyzer's defensive `iron_arena_strdup(...msg...)` arguments hidden inside `iron_diag_emit(...)` calls must be extracted to named locals to make the NULL check reachable"

requirements-completed: [FIX-02]

# Metrics
duration: 57 min
completed: 2026-04-13
---

# Phase 67 Plan 05: FIX-02 Analyzer + Comptime Arena Walkthrough Summary

**Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in the 10 analyzer files (typecheck.c, resolve.c, types.c, capture.c, scope.c, escape.c, concurrency.c, init_check.c, web_await_check.c, web_top_level_loader_check.c) and src/comptime/comptime.c — 94 sites in total — now routes NULL through iron_oom_abort with a function-qualified location literal. 95 total guards (94 new + 1 pre-existing), zero SAFETY annotations, zero behaviour changes, zero regressions.**

## Performance

- **Duration:** 57 min
- **Started:** 2026-04-13T19:16:24Z
- **Completed:** 2026-04-13T20:13:43Z
- **Tasks:** 3
- **Files modified:** 11

## Accomplishments

- **Task 1 (typecheck.c + resolve.c + types.c — 62 sites):** walked the three densest analyzer files in one commit. typecheck.c got 36 new guards (plus 1 from 67-03 remains) spanning resolve_type_annotation, check_expr (LAMBDA / ARRAY_LIT / ENUM_CONSTRUCT with generic mono registration), check_stmt (VAL_DECL tuple destructure, MATCH covered[] + else-note), check_func_decl, check_method_decl, and iron_typecheck's enum pre-pass. resolve.c got 8 guards covering emit_undefined, PATTERN unknown-enum/no-variant/shadow diagnostics, and the ENUM_CONSTRUCT→MethodCall/FieldAccess rewrite (the PROT-04 rank-7/8 allocator path). types.c got 18 guards — every iron_type_make_* constructor's `if (!t) return NULL;` bailout replaced with iron_oom_abort, plus the 6 iron_type_to_string intermediate buffers (NULLABLE/RC/ARRAY/FUNC/ENUM/TUPLE stringifiers) whose previous fallback to static strings ("<null>?", "rc <null>", "[...]", "func(...)", "(...)") silently masked OOM.
- **Task 2 (capture + scope + escape + concurrency + init_check + web_* — 14 sites):** walked the 7 smaller analyzer files. capture.c got 6 guards — each of find_captures / find_spawn_captures / find_pfor_captures has an Iron_CaptureEntry array alloc + a per-name iron_arena_strdup inside the loop, each now guarded (matches grep's "6 hits" despite the audit's original "4" count in CORRECTNESS-AUDIT.md §6 row 20). scope.c got 2 guards (iron_scope_create + iron_symbol_create). The other 5 files are all single-site emit_* functions whose inline iron_arena_strdup was hoisted out of iron_diag_emit and guarded.
- **Task 3 (comptime.c — 18 sites):** walked the full 1298-line comptime evaluator. cval_alloc (the per-value constructor used by cval_int/float/bool/string/null — AUDIT-03 row 20's specifically-called-out site) is now guarded. comptime_cache_read gets 2 guards (val struct + string buf). build_call_trace gets 1 (trace hint strdup). iron_comptime_eval_expr gets 5 (read_file error msg + fbuf, ARRAY_LIT elems, CONSTRUCT field_names + field_vals). iron_comptime_val_to_ast gets 9 (IntLit struct + value, FloatLit struct + value, BoolLit struct, StringLit struct + value, ArrayLit struct + elements array). 67-03's integer-overflow guards (rows 14+15: `__builtin_add_overflow` + INT64_MIN negation) verified still intact via grep.
- Phase-level gate passed: 94 sites total, 95 guards total (the +1 is the pre-existing 67-03 covered[] guard in typecheck.c). Per-file guard count ≥ site count for all 11 files.

## Task Commits

Each task was committed atomically:

1. **Task 1: typecheck.c + resolve.c + types.c** — `5a8417f` (fix)
2. **Task 2: capture + scope + escape + concurrency + init_check + web_*** — `6dde5cb` (fix)
3. **Task 3: comptime.c** — `692acca` (fix)

**Plan metadata:** _pending — will be recorded by docs(67-05): complete FIX-02 analyzer + comptime walkthrough plan_

## Files Created/Modified

- `src/analyzer/typecheck.c` — 36 guard sites; emit_error/emit_warning hoisted; MATCH note hoisted
- `src/analyzer/resolve.c` — 8 guard sites; emit_undefined + PATTERN/ENUM_CONSTRUCT diagnostics hoisted
- `src/analyzer/types.c` — 18 guard sites; every iron_type_make_* constructor + iron_type_to_string stringifier intermediates + tuple_build_mangled_name
- `src/analyzer/capture.c` — 6 guard sites (3 functions × 2 sites each)
- `src/analyzer/scope.c` — 2 guard sites (iron_scope_create + iron_symbol_create)
- `src/analyzer/escape.c` — 1 guard site (emit_err)
- `src/analyzer/concurrency.c` — 2 guard sites (emit_err + emit_warn)
- `src/analyzer/init_check.c` — 1 guard site (emit_uninit_error)
- `src/analyzer/web_await_check.c` — 1 guard site (emit_await_error)
- `src/analyzer/web_top_level_loader_check.c` — 1 guard site (emit_loader_error)
- `src/comptime/comptime.c` — 18 guard sites across 5 functions

## Decisions Made

1. **Guard-by-default over SAFETY annotations (Idiom A)** — Every site got Idiom A (iron_oom_abort guard). Zero SAFETY annotations applied. Every analyzer + comptime allocation is on a user-reachable compilation path with no compile-time bound, so the plan's "when in doubt, guard" rule resolved to "always guard". Matches the 67-04 parser/lexer decision exactly.

2. **Function-qualified location literals with sub-site disambiguators** — Functions with multiple allocations got distinct literals: `typecheck.c:resolve_type_annotation tuple_elems`, `typecheck.c:resolve_type_annotation func_params`, `typecheck.c:resolve_type_annotation type_args`, `typecheck.c:resolve_type_annotation mono`, etc. The disambiguator names either the AST node kind, the variable name, or a semantic fragment. OOM stderr grep resolves to the exact site.

3. **Replace `if (!t) return NULL;` fallbacks with iron_oom_abort in types.c constructors** — Every iron_type_make_* function had a pre-existing soft-NULL return path. Callers already assumed non-NULL (they dereferenced the result without checking). Replacing the `return NULL` with `iron_oom_abort` makes the failure loud and bisectable without changing the callers. Same treatment applied to iron_type_to_string's intermediate-buffer fallbacks, which previously returned static strings ("<null>?", "[...]", "func(...)") on OOM — silently masking OOM as a weird type string.

4. **Hoist inline iron_arena_strdup calls out of iron_diag_emit arguments** — The analyzer's emit_error / emit_warning helpers had their msg strdup inside the diag_emit call, making the NULL check impossible to place without hoisting. Same pattern in resolve.c (3 sites), escape.c, concurrency.c (2 sites), init_check.c, both web_* files, and one typecheck.c MATCH note. Every hoist pulls the strdup into a named local, guards it with iron_oom_abort, then passes the local to diag_emit.

5. **Zero behaviour changes on the green path** — Every guard is strict defensive code on a path that previously dereferenced NULL unconditionally. No test fixtures touched, no error codes added, no AST shape changes. 67-03's integer-overflow guards in comptime.c still intact. The 357-test integration baseline from 67-04 is preserved exactly.

## Deviations from Plan

### Auto-fixed Issues

None — the plan executed exactly as written. No pre-existing latent bugs were surfaced by the walkthrough this time (unlike 67-04's iron_snake_to_camel and iron_lex_string fallbacks). The only minor adjustment was that capture.c had 6 arena-alloc sites, not 4 as the original audit suggested — the plan already anticipated this ("audit flagged 4 sites but grep shows 6 — handle all 6") and all 6 were walked.

---

**Total deviations:** 0
**Impact on plan:** None. Pure walkthrough-and-guard execution.

## Issues Encountered

None. The walkthrough proceeded linearly through each file with per-task grep census + full build + full ctest + full integration run between tasks.

## Authentication Gates

None — fully autonomous walkthrough against local source tree.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- Analyzer + comptime FIX-02 coverage is complete and grep-auditable. The `iron_oom_abort("<file>.c:<function>")` convention is now established across parser, lexer, analyzer, comptime (123 + 94 = 217 total location literals).
- 67-06 (hir + lir + runtime/stdlib, ~105 sites) can now proceed — file-disjoint from this plan, depends only on the iron_oom_abort helper from 67-02.
- 67-07 (FIX-03 cross-arena) and 67-08 (REG-02 crash canaries) remain.
- No blockers. Integration baseline 357/357 preserved; ctest 74/74 preserved.

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Completed: 2026-04-13*

## Self-Check: PASSED

- `.planning/phases/67-correctness-fixes-+-crash-canaries/67-05-SUMMARY.md` exists on disk
- All 11 modified source files exist on disk
- commit `5a8417f` (Task 1: typecheck.c + resolve.c + types.c) present in git log
- commit `6dde5cb` (Task 2: capture + scope + escape + concurrency + init_check + web_*) present in git log
- commit `692acca` (Task 3: comptime.c) present in git log
- Phase-level gate: 94 sites across 11 files, 95 guards total (1 pre-existing from 67-03)
- ctest 74/74 passed, integration 357/357 passed (no regression from 67-04 baseline)
