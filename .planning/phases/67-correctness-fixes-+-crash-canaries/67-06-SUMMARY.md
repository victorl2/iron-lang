---
phase: 67-correctness-fixes-+-crash-canaries
plan: 06
subsystem: correctness
tags: [fix-02, arena, oom, iron_oom_abort, hir, lir, runtime, stdlib, walkthrough]

# Dependency graph
requires:
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: iron_oom_abort helper (67-02) and diagnostics.h wiring
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: 67-04 parser/lexer walkthrough (established idiom conventions)
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: 67-05 analyzer/comptime walkthrough (95 guards, diag_emit hoist pattern)
provides:
  - Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in src/hir/*.c guarded with iron_oom_abort on NULL
  - Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in src/lir/*.c guarded with iron_oom_abort on NULL
  - Every raw malloc/calloc/realloc in src/runtime/iron_string.c + iron_threads.c guarded with iron_oom_abort (AUDIT-06 §21-24 closed)
  - IRON_THREAD_CREATE return-value checks added at 4 call sites in iron_threads.c (AUDIT-03 §32)
  - build_address_list_from_addrinfo in iron_net.c guarded with iron_oom_abort; lookup_host NetError channel preserved as designed fallible path
  - src/parser/printer.c iron_print_ast guarded
  - FIX-02 walkthrough complete end-to-end (67-04 + 67-05 + 67-06 together cover the full 285 M-severity arena audit scope)
affects: [67-07 cross-arena walkthrough, 67-08 crash canaries]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "iron_oom_abort idiom A applied to HIR node constructors: every ARENA_ALLOC → if (!var) iron_oom_abort(\"<file>.c:<fn>\") → dereference"
    - "emit_c.c generated-C vs compiler-internal guard separation: 67-02 guards use `\"emit_c HEAP_ALLOC\"` / `\"emit_c RC_ALLOC\"` literals embedded in generated C strings; 67-06 guards use `\"emit_c.c:<function> [disambiguator]\"` literals for compiler-internal arena scratch. grep can distinguish the two classes by prefix"
    - "Runtime silent-fallback replacement pattern: Phase 65 string methods had silent empty-string / self-return fallbacks on OOM that masked failures as weird values — replaced with iron_oom_abort for consistent OOM reporting. Deliberate, documented behaviour change"
    - "IRON_THREAD_CREATE return-value checking: the cross-platform macro returns int (pthread_create / iron__win_thread_create) — add `if (IRON_THREAD_CREATE(...) != 0) iron_oom_abort(...)` at every call site since the runtime has no other panic channel for EAGAIN scheduler failure"
    - "const char * → char * drift fix in emit_c/emit_helpers: arena_strdup results feeding arrput(ctx->emitted_tuples, ...) must be typed as char * (not const char *) because stb_ds emitted_tuples is declared char **"

key-files:
  created: []
  modified:
    - src/hir/hir.c (45 sites, 46 guards — 45 HIR constructors + 1 malloc in iron_hir_module_create)
    - src/hir/hir_lower.c (16 sites, 16 guards — AST-to-HIR lowering with slot_field hoist)
    - src/hir/hir_to_lir.c (12 sites, 12 guards — HIR-to-LIR walker; Phase 66 IRON_NODE_ASSERT_KIND preserved)
    - src/hir/hir_verify.c (0 sites — audit confirmed no arena-alloc sites)
    - src/lir/lir.c (6 sites, 6 guards — LIR node constructors)
    - src/lir/lir_optimize.c (16 sites, 16 guards — pass scratch arrays)
    - src/lir/emit_c.c (24 arena sites, 33 guards total — 24 new FIX-02 arena guards with emit_c.c: prefix + 8 67-02 generated-C guards still intact + 1 pre-existing iface_lower guard)
    - src/lir/emit_helpers.c (8 sites, 8 guards — emit_ensure_optional / emit_ensure_tuple / emit_type_to_c scratch)
    - src/lir/emit_structs.c (5 sites, 5 guards — includes AUDIT-06 §17 indirect variant malloc; Phase 66-05 IRON_NODE_ASSERT_KIND x3 preserved)
    - src/lir/emit_split.c (2 sites, 2 guards)
    - src/lir/emit_fusion.c (4 calloc sites, 4 guards — AUDIT-06 §15, not arena_alloc; flat/split cur_var sites found by grep)
    - src/lir/emit_web.c (1 site, 1 guard)
    - src/lir/web_main_loop_split.c (3 sites, 3 guards — capture_entries + name strdup + split meta)
    - src/lir/value_range.c (2 sites, 2 guards — __builtin_*_overflow paths left alone)
    - src/lir/layout_analysis.c (1 site, 1 guard)
    - src/parser/printer.c (1 site, 1 guard — iron_print_ast silent-fallback replaced)
    - src/runtime/iron_string.c (9 malloc sites, 9 guards — all FIX-02 tagged)
    - src/runtime/iron_threads.c (17 malloc sites + 4 IRON_THREAD_CREATE = 21 guards)
    - src/stdlib/iron_net.c (3 malloc sites, 1 new guard — other 2 are Phase 66-05 fix + lookup_host NetError channel)

key-decisions:
  - "Guard-by-default idiom A applied to every hir/lir/runtime/stdlib OOM site (~150 new guards across 19 files). Zero SAFETY annotations. Matches 67-04 and 67-05 decision exactly"
  - "Task 1 (hir walkthrough) was already committed at session start (commit d005c72) — session picked up with Task 2 (lir files) pre-edited but unbuilt. Recovered the working state by fixing 7 const char * → char * drift errors in emit_c.c (5 sites) and emit_helpers.c (2 sites), applying Idiom A to printer.c, then proceeding to Task 3"
  - "iron_string.c silent-truncation/empty-string fallbacks replaced with iron_oom_abort — deliberate behaviour change. Every edit tagged with `FIX-02: replace ... fallback with iron_oom_abort ...` comment for grep auditability. 9 sites affected"
  - "iron_threads.c Iron_pool_create soft-NULL returns replaced with iron_oom_abort because (a) callers already dereferenced the return without NULL checks, and (b) iron_threads_init would otherwise propagate a NULL Iron_global_pool into a double-faulting code path. AUDIT-06 §22-24 closed by the Iron_pool_create + Iron_elastic_pool_create edits transitively"
  - "IRON_THREAD_CREATE failure guards use iron_oom_abort even though the failure (typically EAGAIN from the scheduler) is not strictly an OOM condition. The runtime has no other panic channel today; iron_oom_abort is the closest noreturn helper and the literal message disambiguates the failure class. Same decision pattern documented in 67-06-PLAN.md interface block"
  - "iron_net.c Iron_net_lookup_host DnsJob calloc + Iron_poolwait_create NULL check deliberately NOT replaced with iron_oom_abort — these sites surface NO_MEMORY through the designed NetError channel (Iron_Tuple__Address__NetError). Replacing them would break a documented fallible-runtime contract. Only build_address_list_from_addrinfo (which had a silent empty-list fallback) got the iron_oom_abort treatment"
  - "Phase 66-05 local_recv_buf fix at iron_net.c lines 611-619 left untouched (4 grep hits preserved). Phase 66 IRON_NODE_ASSERT_KIND counts preserved exactly: typecheck.c=16, resolve.c=6, emit_structs.c=3, hir_to_lir.c=0, iron_net.c=0. 67-02 emit_c.c generated-C guards (HEAP_ALLOC/RC_ALLOC/boxed-ADT/interp-buf/closure-env x3/parallel-for-ctx) all preserved — 8 grep hits intact"

patterns-established:
  - "HIR constructor FIX-02 idiom: every iron_hir_stmt_* / iron_hir_expr_* / iron_hir_block / iron_hir_module constructor gets a one-line iron_oom_abort after its ARENA_ALLOC and before the kind/field assignments"
  - "LIR emitter FIX-02 idiom: compiler-internal arena scratch sites use `\"<file>.c:<function> [disambiguator]\"` literals; generated-C strings emitted into the output .c file keep the shorter `\"emit_c <class>\"` literal format from 67-02"
  - "Runtime fallible → infallible migration: Phase 65 string methods that silently truncated / returned empty on OOM now deliberately abort via iron_oom_abort. The Iron-side semantics of these methods were always 'infallible' — the C-side fallback was a hidden correctness hole"
  - "Stdlib dual-channel OOM: when a stdlib function has a designed fallible error channel (like Iron_Tuple__Address__NetError for Net.lookup_host), leave the soft-NULL path alone. When it has only silent-fallback behaviour, replace with iron_oom_abort"

requirements-completed: [FIX-02]

# Metrics
duration: 59 min
completed: 2026-04-13
---

# Phase 67 Plan 06: FIX-02 HIR + LIR + Runtime/Stdlib Arena Walkthrough Summary

**Every iron_arena_alloc / ARENA_ALLOC / iron_arena_strdup call-site in the HIR + LIR subsystems (19 files, 146 arena sites) and every raw malloc/calloc/realloc in the runtime + stdlib subsystems flagged by AUDIT-06 §21-24 (3 files, 29 malloc sites + 4 IRON_THREAD_CREATE return checks) now routes NULL / non-zero through iron_oom_abort with a function-qualified location literal. Combined with 67-04 (parser/lexer, 123 sites) and 67-05 (analyzer/comptime, 94 sites), this plan closes FIX-02 in its entirety — all 285 M-severity arena sites enumerated in CORRECTNESS-AUDIT.md §6 are now guarded.**

## Performance

- **Duration:** 59 min
- **Started:** 2026-04-13T17:49:45Z (Task 1 commit timestamp)
- **Completed:** 2026-04-13T21:49:30Z (plan verification complete)
- **Tasks:** 3
- **Files modified:** 22

## Accomplishments

- **Task 1 (HIR subsystem — 73 sites):** walked src/hir/hir.c (45 HIR-node constructors + 1 malloc in iron_hir_module_create = 46 guards), src/hir/hir_lower.c (16 guards — AST-to-HIR lowering with the slot_field strdup hoist idiom from 67-05), src/hir/hir_to_lir.c (12 guards — HIR-to-LIR walker; Phase 66 IRON_NODE_ASSERT_KIND sites untouched), and confirmed src/hir/hir_verify.c has zero arena-alloc sites (audit's original prediction). Commit d005c72.
- **Task 2 (LIR subsystem + printer — 12 files, 68 sites):** walked lir.c (6), lir_optimize.c (16), emit_c.c (16 new arena guards bringing the file to 24 arena-scoped guards on top of 67-02's 8 generated-C guards = 32 total iron_oom_abort hits), emit_helpers.c (8), emit_structs.c (5 — Phase 66-05 IRON_NODE_ASSERT_KIND x3 preserved), emit_split.c (2), emit_fusion.c (4 calloc guards — AUDIT-06 §15), emit_web.c (1), web_main_loop_split.c (3), value_range.c (2 — __builtin_*_overflow paths untouched), layout_analysis.c (1), and printer.c (1 — silent-fallback replacement). Commit 06cfd0b.
- **Task 3 (runtime + stdlib raw malloc — 3 files, 29 alloc + 4 IRON_THREAD_CREATE):** walked iron_string.c (9 guards replacing Phase 65 silent-truncation/empty-string fallbacks), iron_threads.c (21 guards covering Iron_pool_create / Iron_elastic_pool_create / pool_queue_grow / Iron_pool_destroy / Iron_poolwait_create / Iron_handle_create / iron_handle_create_self_ref / Iron_channel_create / Iron_mutex_create + 4 IRON_THREAD_CREATE return-checks), and iron_net.c (1 guard for build_address_list_from_addrinfo; lookup_host's NetError channel preserved as designed fallible path). Commit 85e925c.
- Phase-level gate passed: for every file in the plan's files_modified list, `grep -c 'iron_oom_abort('` ≥ `grep -c 'iron_arena_alloc\|ARENA_ALLOC\|iron_arena_strdup'`. ctest 74/74 passed. Integration 357/357 passed (baseline preserved from 67-04/67-05).
- Phase 66 IRON_NODE_ASSERT_KIND counts preserved exactly (typecheck.c=16, resolve.c=6, emit_structs.c=3, hir_to_lir.c=0, iron_net.c=0). 67-02 emit_c.c generated-C guards (8 grep hits) untouched. 67-02 iron_runtime.h LIST/MAP/SET guards (16 hits) untouched. Phase 66-05 iron_net.c local_recv_buf fix (4 grep hits) intact.

## Task Commits

Each task was committed atomically:

1. **Task 1: HIR subsystem (hir.c + hir_lower.c + hir_to_lir.c + hir_verify.c)** — `d005c72` (fix)
2. **Task 2: LIR subsystem (11 files) + printer.c** — `06cfd0b` (fix)
3. **Task 3: Runtime + stdlib (iron_string.c + iron_threads.c + iron_net.c residuals)** — `85e925c` (fix)

**Plan metadata:** _pending — will be recorded by docs(67-06): complete FIX-02 hir/lir/runtime walkthrough plan_

## Files Created/Modified

**HIR (4 files):**
- `src/hir/hir.c` — 46 guards (45 HIR constructors + 1 iron_hir_module_create malloc)
- `src/hir/hir_lower.c` — 16 guards (AST-to-HIR lowering)
- `src/hir/hir_to_lir.c` — 12 guards (HIR-to-LIR walker)
- `src/hir/hir_verify.c` — 0 edits (no arena sites; documented in Task 1 commit)

**LIR (11 files):**
- `src/lir/lir.c` — 6 guards (LIR node constructors)
- `src/lir/lir_optimize.c` — 16 guards (optimizer scratch)
- `src/lir/emit_c.c` — 24 new arena guards (with `emit_c.c:` prefix) + 8 67-02 generated-C guards (preserved) + 1 pre-existing iface_lower guard
- `src/lir/emit_helpers.c` — 8 guards (dedupe registry + type-to-c)
- `src/lir/emit_structs.c` — 5 guards (+ 3 Phase 66-05 IRON_NODE_ASSERT_KIND preserved)
- `src/lir/emit_split.c` — 2 guards
- `src/lir/emit_fusion.c` — 4 calloc guards (AUDIT-06 §15)
- `src/lir/emit_web.c` — 1 guard
- `src/lir/web_main_loop_split.c` — 3 guards
- `src/lir/value_range.c` — 2 guards
- `src/lir/layout_analysis.c` — 1 guard

**Parser (1 file):**
- `src/parser/printer.c` — 1 guard (iron_print_ast)

**Runtime (2 files):**
- `src/runtime/iron_string.c` — 9 guards (all 9 malloc sites tagged FIX-02)
- `src/runtime/iron_threads.c` — 21 guards (17 malloc/calloc/realloc sites + 4 IRON_THREAD_CREATE return checks)

**Stdlib (1 file):**
- `src/stdlib/iron_net.c` — 1 new guard (build_address_list_from_addrinfo; 2 other malloc sites preserved as fallible NetError paths or Phase 66-05 fix)

## Decisions Made

1. **Guard-by-default Idiom A across every file (~150 new guards)** — Zero SAFETY annotations applied. Every site is on a user-reachable compilation or runtime path with no compile-time bound, so the plan's "when in doubt, guard" rule resolved to "always guard". Matches 67-04 and 67-05 decisions exactly.

2. **Recovery from mid-Task-2 state** — Task 1 (HIR) was already committed at session start as commit d005c72, and Task 2's LIR file edits were in progress but uncommitted and unbuilt. The session picked up by running the build, fixing 7 `const char * → char *` drift errors in emit_c.c (5 sites) and emit_helpers.c (2 sites) where arena_strdup results are fed into `arrput(ctx->emitted_tuples, ...)` (declared `char **` in emit_helpers.h — `const char *` locals discard qualifiers under `-Werror=incompatible-pointer-types-discards-qualifiers`). Also added the missing printer.c guard and re-verified per-file census before committing Task 2.

3. **Replace iron_string.c silent-fallback behaviour with iron_oom_abort (9 sites)** — Every iron_string.c built-in method had a Phase 65 silent fallback that masked OOM as a weird value (empty string, self-return, SSO truncation). FIX-02 replaces these with iron_oom_abort. Deliberate behaviour change. Every edit tagged with a `FIX-02: replace ... fallback with iron_oom_abort` comment so a grep for `FIX-02` in iron_string.c returns 9 hits (one per edit).

4. **iron_threads.c Iron_pool_create soft-NULL returns → iron_oom_abort** — The pre-Phase-67 implementation returned NULL on malloc failure, but every caller (iron_threads_init, iron_handle_create, etc.) dereferenced the return without a NULL check. Rather than audit every caller and add NULL checks, replace the soft-NULL path with iron_oom_abort. Same treatment for Iron_elastic_pool_create, Iron_handle_create, iron_handle_create_self_ref, Iron_channel_create, Iron_mutex_create, Iron_poolwait_create.

5. **IRON_THREAD_CREATE return-value checking (4 sites)** — Pre-Phase-67 the macro's return value was ignored. FIX-02 / AUDIT-03 §32 adds `if (IRON_THREAD_CREATE(...) != 0) iron_oom_abort(...)` at 4 call sites (Iron_pool_create worker loop, pool_spawn_elastic_worker_locked, Iron_handle_create, iron_handle_create_self_ref). The failure class is typically EAGAIN (scheduler out of task slots), not strictly OOM, but iron_oom_abort is the closest noreturn helper the runtime has — the literal message disambiguates. Documented in the plan's interface block.

6. **iron_net.c dual-channel OOM decision** — The Iron_net_lookup_host path has a *designed* NetError channel (Iron_Tuple__Address__NetError with IRON_ERR_NET_NO_MEMORY). Replacing the DnsJob calloc + Iron_poolwait_create NULL checks with iron_oom_abort would break that documented fallible contract. Only `build_address_list_from_addrinfo` — which had a silent empty-list fallback indistinguishable from "lookup succeeded with zero matching entries" — got the iron_oom_abort treatment. This distinction is the plan's "stdlib functions have no error channel today" rule applied with nuance: some do, most don't.

7. **Zero behaviour changes on the green path** — Every guard is strict defensive code on a path that previously either dereferenced NULL or silently fell back to a masked-failure value. No test fixtures touched, no error codes added, no AST or runtime struct shape changes. Phase 65 tests that exercise the now-aborting silent-truncation paths still pass because no test allocates enough memory to exhaust the heap.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] const char * → char * drift fix in emit_c.c and emit_helpers.c**
- **Found during:** Task 2 (first build attempt after pre-existing uncommitted LIR edits)
- **Issue:** 7 sites where `arena_strdup` results were captured into `const char *` locals and then passed to `arrput(ctx->emitted_tuples, ...)` / `arrput(ctx->emitted_optionals, ...)`. emitted_tuples / emitted_optionals are declared `char **` in `src/lir/emit_helpers.h:63`, so the stb_ds `stbds_arrput` macro expansion assigns the `const char *` into a `char *` slot — which `-Werror=incompatible-pointer-types-discards-qualifiers` rejects. 5 sites in emit_c.c (net_tuple, udp_tuple, addr_list, addr_tuple, ip_tuple) and 2 sites in emit_helpers.c (emit_ensure_optional, emit_ensure_tuple).
- **Fix:** Flipped the const qualifier off at all 7 sites (`const char *foo_copy` → `char *foo_copy`).
- **Files modified:** src/lir/emit_c.c, src/lir/emit_helpers.c
- **Verification:** Build clean after fix; ctest 74/74, integration 357/357 still green.
- **Committed in:** 06cfd0b (folded into the Task 2 commit rather than a separate fix commit — the drift was part of recovering the mid-Task-2 state)

**2. [Rule 3 - Blocking] printer.c missing guard**
- **Found during:** Task 2 (per-file census found printer.c had 1 arena site with 0 guards)
- **Issue:** The pre-existing Task 2 edits added guards across the 11 LIR files but missed `src/parser/printer.c:iron_print_ast` which the plan explicitly calls out as "1 site; missed from 67-04's parser-subsystem scope because printer.c is AST-printer, not parser-proper".
- **Fix:** Added `if (!out) iron_oom_abort("printer.c:iron_print_ast");` after the arena_alloc, replaced the silent `return out ? out : (char *)"";` fallback with direct `return out;` since we can no longer reach the NULL path.
- **Files modified:** src/parser/printer.c
- **Verification:** grep census reports 1 site / 1 guard; full build + tests green.
- **Committed in:** 06cfd0b (Task 2 commit)

---

**Total deviations:** 2 auto-fixed (both Rule 3 - Blocking, both in Task 2 recovery)
**Impact on plan:** Both auto-fixes essential for Task 2 completion. No scope creep — printer.c was explicitly called out in the plan's file list, and the const-qualifier fix was a mechanical restoration of the mid-edit LIR state to a buildable shape.

## Issues Encountered

- **Task 2 pre-existing uncommitted state:** At session start the working tree had 11 LIR files modified but uncommitted, and the build was broken (`-Werror=incompatible-pointer-types-discards-qualifiers` on 5 emit_c.c sites + 2 emit_helpers.c sites). Recovered by diagnosing the const-char drift, applying the surgical const → non-const fix, adding the missing printer.c guard, and proceeding with Task 3. Task 1 was already committed cleanly before the session started.
- **iron_threads_init does NOT need a post-hoc NULL check** on Iron_global_pool / Iron_io_pool even though the plan mentioned adding one — Iron_pool_create now aborts instead of returning NULL, so the post-creation check is structurally unreachable. Left untouched (the plan's Step 4 §3 mentioned this as a possible alternative to updating callers; the Iron_pool_create iron_oom_abort edits subsume it transitively).

## Authentication Gates

None — fully autonomous walkthrough against local source tree.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- **FIX-02 is complete** end-to-end across parser, lexer, analyzer, comptime, HIR, LIR, runtime, and stdlib. Combined guard counts:
  - 67-04 (parser+lexer): 123 guards
  - 67-05 (analyzer+comptime): 95 guards
  - 67-06 (hir+lir+runtime+stdlib): ~150 guards
  - **Total FIX-02 coverage: ~368 guards** across ~40 files, comfortably exceeding the audit's 285 M-severity site count (the excess comes from the 67-02 runtime macros + emit_c.c generated-C guards + IRON_THREAD_CREATE return checks which weren't in the original audit's arena-alloc count).
- FIX-02 requirement can now be marked `[x]` in REQUIREMENTS.md.
- 67-07 (FIX-03 cross-arena AUDIT-04 walkthrough, 16 M rows) ready to start — depends only on the iron_oom_abort helper from 67-02 and is file-disjoint from the FIX-02 walkthroughs.
- 67-08 (REG-02 crash canaries, 15 fixtures) waits for 67-07 to land so each canary can exercise a guard or integer-overflow path that's actually in place.
- No blockers. Integration baseline 357/357 preserved; ctest 74/74 preserved.

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Completed: 2026-04-13*

## Self-Check: PASSED

- `.planning/phases/67-correctness-fixes-+-crash-canaries/67-06-SUMMARY.md` exists on disk
- All 18 modified source files exist on disk (hir.c, hir_lower.c, hir_to_lir.c, lir.c, lir_optimize.c, emit_c.c, emit_helpers.c, emit_structs.c, emit_split.c, emit_fusion.c, emit_web.c, web_main_loop_split.c, value_range.c, layout_analysis.c, printer.c, iron_string.c, iron_threads.c, iron_net.c)
- commit `d005c72` (Task 1: HIR subsystem) present in git log
- commit `06cfd0b` (Task 2: LIR + printer.c) present in git log
- commit `85e925c` (Task 3: runtime + stdlib) present in git log
- Phase-level gate: per-file `grep -c 'iron_oom_abort('` ≥ per-file `grep -c 'iron_arena_alloc\|ARENA_ALLOC\|iron_arena_strdup'` across all 18 files
- Phase 66 IRON_NODE_ASSERT_KIND counts preserved (typecheck.c=16, resolve.c=6, emit_structs.c=3, hir_to_lir.c=0, iron_net.c=0)
- 67-02 emit_c.c generated-C guards intact (8 grep hits for HEAP_ALLOC/RC_ALLOC/boxed ADT/interp string/closure env A,B,C/parallel-for ctx)
- Phase 66-05 iron_net.c local_recv_buf fix intact (4 grep hits)
- ctest 74/74 passed, integration 357/357 passed (no regression from 67-05 baseline)
