---
phase: 67-correctness-fixes-+-crash-canaries
plan: 02
subsystem: runtime + codegen + diagnostics
tags: [oom, alloc-fail, iron_oom_abort, FIX-01, FIX-02, IRON_LIST_IMPL, IRON_MAP_IMPL, IRON_SET_IMPL, HEAP_ALLOC, RC_ALLOC, wasm-w1, emit_web, correctness-audit]

# Dependency graph
requires:
  - phase: 66-structural-protections-linux-release-ci
    provides: iron_ice + IRON_NODE_ASSERT_KIND diagnostics infrastructure that iron_oom_abort sits alongside; Phase 66-03 PROT-04 fixture template (4-section doc-comment) that the two new integration fixtures adopt
  - phase: 67-correctness-fixes-+-crash-canaries
    provides: 67-01 audit-status verification doc that routed ranks 1-4 + Wasm-W1 to this plan; 67-CONTEXT.md LOCKED decision that iron_oom_abort lives in the diagnostics layer
provides:
  - "iron_oom_abort(const char *where) noreturn helper — canonical OOM abort path for contexts with no error channel (runtime macros, generated C, compiler-internal fatal allocs)"
  - "IRON_LIST_IMPL / IRON_MAP_IMPL / IRON_SET_IMPL macro families fully NULL-checking every malloc + realloc via iron_oom_abort with suffix-qualified stderr location"
  - "int64_t capacity-doubling wraparound guard on all three _push / _put / _add collection grow paths (audit row 18/19)"
  - "emit_c.c HEAP_ALLOC + RC_ALLOC + boxed-ADT + interp-buffer + 3 closure-env + parallel-for-ctx malloc sites all emitting iron_oom_abort guards into generated C"
  - "emit_web.c main-loop FrameState wrapper guard covering Wasm-W1 from 67-01 re-audit"
  - "2 C unit tests exercising OOM paths via fork+pipe+SIGABRT assertions"
  - "2 Iron integration fixtures with 4-section doc-comment headers exercising heap and rc keyword allocation paths"
affects: [67-03, 67-04, 67-05, 67-06, 67-07, 67-08]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "iron_oom_abort(where): noreturn helper in src/diagnostics/diagnostics.h + src/runtime/iron_oom.c — canonical OOM abort path adjacent to iron_ice"
    - "IRON_LIST/MAP/SET _push/_put/_add: int64_t new_cap temporary → wraparound check → named realloc temp → NULL check → commit to self-> — safe replacement of in-place realloc"
    - "emit_c.c malloc guard: emit_val + string literal between iron_strbuf_appendf malloc line and dereference line — compile-time location literal identifies call-site in stderr"
    - "Build-path parity: every new runtime .c file is wired into both iron_runtime CMake target AND src/cli/build.c build_src_list argv_buf (59-01c convention) — preserved by Task 1 commit"

key-files:
  created:
    - "src/runtime/iron_oom.c — iron_oom_abort implementation (kept out of diagnostics.c to avoid dragging parser/ast.h + stb_ds into iron_runtime static library)"
    - "tests/unit/test_alloc_list_push_oom.c — fork-based SIGABRT + stderr assertion for Iron_List_int64_t_push capacity-overflow arm"
    - "tests/unit/test_alloc_map_put_oom.c — fork-based SIGABRT + stderr assertion for Iron_Map_Iron_String_int64_t_create_with_capacity malloc-NULL arm"
    - "tests/integration/null_heap_alloc_malloc.iron (+ .expected) — heap keyword HEAP_ALLOC guard fixture with 4-section doc-comment header"
    - "tests/integration/null_rc_alloc_malloc.iron (+ .expected) — rc keyword RC_ALLOC guard fixture with 4-section doc-comment header"
  modified:
    - "src/diagnostics/diagnostics.h — iron_oom_abort declaration adjacent to iron_ice, marked __attribute__((noreturn))"
    - "src/runtime/iron_runtime.h — #include diagnostics/diagnostics.h so generated user C picks it up transitively; all three IRON_LIST/MAP/SET _IMPL macro bodies rewritten with OOM + capacity-overflow guards"
    - "src/lir/emit_c.c — HEAP_ALLOC, RC_ALLOC, boxed ADT, interp buffer, 3 closure env sites, parallel-for ctx malloc sites emit iron_oom_abort guards into generated C"
    - "src/lir/emit_web.c — ew_emit_main_wrapper emits FrameState malloc guard (Wasm-W1)"
    - "CMakeLists.txt — iron_runtime target now links src/runtime/iron_oom.c"
    - "src/cli/build.c — build_src_list / free_src_list / invoke_clang argv_buf lanes (unix + windows) include iron_oom.c in user-facing ironc binaries"
    - "tests/unit/CMakeLists.txt — adds test_alloc_list_push_oom + test_alloc_map_put_oom executables"
    - "docs/regression-fixtures.md — new Phase 67 Plan 02 section listing the 2 new integration fixtures alongside Phase 66-03 PROT-04 entries"
    - ".planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md — ranks 1-4 + Wasm-W1 moved from OPEN to DONE with commit hashes + evidence"

key-decisions:
  - "iron_oom_abort lives in src/runtime/iron_oom.c (not diagnostics.c) to keep iron_runtime static library self-contained — diagnostics.c drags parser/ast.h + stb_ds which iron_runtime tests must not link. Declaration still in diagnostics.h adjacent to iron_ice so call-sites use a unified header."
  - "All IRON_LIST/MAP/SET _push/_put/_add paths capture realloc into a named temporary BEFORE committing to self->, so a NULL return leaves the original backing pointer intact and the OOM abort fires with no side effects on the live collection."
  - "Capacity-doubling wraparound check (new_cap < self->capacity) sits BEFORE the realloc call so we abort with a bisectable 'capacity overflow' suffix instead of letting the int64_t wrap negative and feeding (size_t)-4 to realloc."
  - "emit_c.c malloc-guard location literals identify the call-site structurally ('emit_c HEAP_ALLOC', 'emit_c closure env (A|B|C)') — generated C cannot use __FILE__ because that points at emit_c.c, not the user's source file. Distinct literals per site let stderr grep bisect which emitter path failed."
  - "test_alloc_list_push_oom.c exercises the capacity-overflow arm (not the realloc-NULL arm) because forcing realloc to return NULL portably requires LD_PRELOAD or link-time __wrap_realloc, which the existing harness does not support. The overflow arm calls the same iron_oom_abort with the same stderr contract, so it covers the helper wiring equivalently."
  - "test_alloc_map_put_oom.c exercises _create_with_capacity (not _put) because _put does a linear scan over self->keys[0..count] BEFORE the capacity check — forcing count == capacity == INT64_MAX/2+1 segfaults on the scan before reaching the overflow guard. _create_with_capacity has no scan prerequisite."
  - "Generated C prelude already includes runtime/iron_runtime.h (both emit_c.c and emit_web.c), and iron_runtime.h transitively includes diagnostics/diagnostics.h after Task 1 — so zero additional include wiring is needed in the emitters to make iron_oom_abort reachable from user binaries."

patterns-established:
  - "FIX-01 OOM guard style: named realloc temporary + NULL check via iron_oom_abort + commit to live pointer only after NULL check passes. Reusable for every FIX-02 allocation site in 67-04/67-05/67-06."
  - "Integer-overflow-before-alloc guard: compute new_cap into local → check wraparound → abort with 'capacity overflow' suffix → only then call realloc/malloc. Reusable for any capacity-doubling growth path."
  - "Generated-C OOM guard: stringize the call-site identifier into the location literal so stderr is bisectable without a debugger. Pattern scales to any future emit_c.c malloc site."

requirements-completed: [FIX-01, FIX-02]

# Metrics
duration: 2h 2min
completed: 2026-04-13
---

# Phase 67 Plan 02: FIX-01 H-severity alloc-fail walkthrough + iron_oom_abort helper Summary

**iron_oom_abort noreturn helper wired into IRON_LIST/MAP/SET macro families and every emit_c.c + emit_web.c generated-code malloc site, closing audit ranks 1-4 + Wasm-W1 with bisectable stderr diagnostics replacing silent SIGSEGV on OOM.**

## Performance

- **Duration:** 2h 2min (wall clock from 97a686e to b2e1555)
- **Started:** 2026-04-13T15:26:51Z (Task 1 commit time)
- **Completed:** 2026-04-13T17:28:33Z (Task 3 commit time)
- **Tasks:** 3 (iron_oom_abort helper + runtime macros + emit_c/emit_web)
- **Files modified:** 12 (5 created + 7 modified)

## Accomplishments

- Landed the canonical `iron_oom_abort(const char *where)` noreturn helper in the diagnostics layer, reachable from every compiled Iron binary through the existing `runtime/iron_runtime.h` → `diagnostics/diagnostics.h` include chain. Helper prints "iron: out of memory at <where>\n" to stderr, flushes, and calls abort(3) — distinct from `iron_ice` so downstream telemetry can tell compiler-internal ICEs apart from legitimate runtime OOM.
- Rewrote the three collection macro families (`IRON_LIST_IMPL`, `IRON_MAP_IMPL`, `IRON_SET_IMPL`) so every `_create_with_capacity`, `_clone`, and `_push`/`_put`/`_add` NULL-checks every malloc and realloc call. `_push`/`_put`/`_add` additionally guard int64_t capacity-doubling wraparound before the realloc — audit row 18/19 closed as a side effect.
- Rewrote `src/lir/emit_c.c` `IRON_LIR_HEAP_ALLOC` + `IRON_LIR_RC_ALLOC` cases to emit `if (!_vN) iron_oom_abort("emit_c HEAP_ALLOC|RC_ALLOC");` between the malloc call and the `*_vN =` dereference, closing audit ranks 3 + 4. Same treatment applied to the six M-severity sibling malloc sites in the same file (boxed ADT, interp buffer, 3 closure env, parallel-for ctx) — every `malloc(` in `emit_c.c` is now NULL-checked.
- Rewrote `src/lir/emit_web.c` `ew_emit_main_wrapper` to emit the same guard on the FrameState malloc, closing Wasm-W1 from 67-01's Wasm re-audit. Same `iron_oom_abort` helper, distinct "emit_web main-loop FrameState" location literal.
- Added 2 C unit tests (`test_alloc_list_push_oom.c`, `test_alloc_map_put_oom.c`) that fork a child, exercise an OOM arm, redirect stderr to a pipe, and assert the child exited via SIGABRT with the expected "iron: out of memory at" diagnostic.
- Added 2 Iron integration fixtures (`null_heap_alloc_malloc.iron`, `null_rc_alloc_malloc.iron`) with full 4-section doc-comment headers (Motivating Incident / Layout Diagram / Fix Summary / Severity) following the Phase 66-03 PROT-04 template. Each exercises the respective keyword's generated-C path and is grep-verifiable via `ironc build --debug-build` + inspection of `.iron-build/main.c`.
- End-to-end verification: built `null_heap_alloc_malloc.iron` with `--debug-build`, grepped `.iron-build/main.c`, confirmed `if (!_v4) iron_oom_abort("emit_c HEAP_ALLOC");` is literally present in the emitted C. Same verification for RC_ALLOC.
- Updated `docs/regression-fixtures.md` with a new Phase 67 Plan 02 section listing both fixtures alongside the Phase 66-03 PROT-04 entries.
- Updated `.planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` — ranks 1-4 + Wasm-W1 moved from OPEN to DONE with commit hashes and evidence. Summary line updated: 17 DONE / 3 OPEN (was 13 DONE / 7 OPEN).

## Task Commits

1. **Task 1: Add iron_oom_abort helper to diagnostics layer** — `97a686e` (feat)
   - Declaration in src/diagnostics/diagnostics.h marked `__attribute__((noreturn))`
   - Implementation in new src/runtime/iron_oom.c
   - Wired into iron_runtime CMake target + src/cli/build.c build_src_list (ironc build-path parity)
   - `#include "diagnostics/diagnostics.h"` added to src/runtime/iron_runtime.h for transitive reachability from generated C
2. **Task 2: Guard IRON_LIST/MAP/SET macro families + 2 OOM unit tests** — `61f0a8c` (feat)
   - 16 iron_oom_abort call-sites across the three _IMPL macro bodies
   - 3 capacity-overflow guards (one per collection family)
   - test_alloc_list_push_oom.c + test_alloc_map_put_oom.c with fork + pipe + SIGABRT assertions
   - tests/unit/CMakeLists.txt wiring
3. **Task 3: Guard emit_c.c + emit_web.c generated malloc sites + 2 fixtures** — `b2e1555` (feat)
   - 9 iron_oom_abort call-sites in emit_c.c (HEAP_ALLOC, RC_ALLOC, boxed ADT, interp buffer, 3 closure env, parallel-for ctx — total 8 unique sites, plus 1 in a comment cross-reference)
   - 1 iron_oom_abort call-site in emit_web.c (Wasm-W1)
   - null_heap_alloc_malloc.iron/.expected + null_rc_alloc_malloc.iron/.expected
   - docs/regression-fixtures.md Phase 67 Plan 02 section

**Plan metadata:** pending — will land with the SUMMARY + STATE.md + ROADMAP.md + REQUIREMENTS.md commit below.

## Files Created/Modified

### Created
- `src/runtime/iron_oom.c` — 43 lines. iron_oom_abort implementation kept out of diagnostics.c to avoid dragging parser/ast.h + stb_ds into the iron_runtime static library.
- `tests/unit/test_alloc_list_push_oom.c` — 137 lines. Fork-based SIGABRT assertion for Iron_List_int64_t_push capacity-overflow arm.
- `tests/unit/test_alloc_map_put_oom.c` — 135 lines. Fork-based SIGABRT assertion for Iron_Map_Iron_String_int64_t_create_with_capacity malloc-NULL arm.
- `tests/integration/null_heap_alloc_malloc.iron` + `.expected` — Minimal `heap Point(3, 4)` fixture with 4-section doc-comment header exercising HEAP_ALLOC guard.
- `tests/integration/null_rc_alloc_malloc.iron` + `.expected` — Minimal `rc Config(5, 3000)` fixture with 4-section doc-comment header exercising RC_ALLOC guard.

### Modified
- `src/diagnostics/diagnostics.h` — iron_oom_abort declaration (Task 1).
- `src/runtime/iron_runtime.h` — #include diagnostics/diagnostics.h (Task 1); IRON_LIST/MAP/SET _IMPL bodies rewritten with OOM + capacity-overflow guards (Task 2). 16 iron_oom_abort call-sites.
- `src/lir/emit_c.c` — HEAP_ALLOC, RC_ALLOC, boxed ADT, interp buffer, 3 closure env sites, parallel-for ctx all emit iron_oom_abort guards (Task 3). 9 iron_oom_abort occurrences.
- `src/lir/emit_web.c` — ew_emit_main_wrapper emits FrameState malloc guard (Task 3, Wasm-W1). 1 iron_oom_abort occurrence.
- `CMakeLists.txt` — iron_runtime target wired to src/runtime/iron_oom.c (Task 1).
- `src/cli/build.c` — build_src_list argv_buf lanes wired to iron_oom.c (Task 1, ironc build-path parity).
- `tests/unit/CMakeLists.txt` — test_alloc_list_push_oom + test_alloc_map_put_oom executables + add_test (Task 2).
- `docs/regression-fixtures.md` — Phase 67 Plan 02 section (Task 3).
- `.planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` — ranks 1-4 + Wasm-W1 DONE with commit hashes.

## Grep Census of iron_oom_abort

As of plan completion:

| File | Count |
|------|-------|
| src/diagnostics/diagnostics.h | 3 (declaration + 2 doc refs) |
| src/runtime/iron_oom.c | 2 (function definition + internal ref) |
| src/runtime/iron_runtime.h | 16 (LIST + MAP + SET × _create/_clone/_grow branches, including 3 capacity-overflow paths) |
| src/lir/emit_c.c | 9 (HEAP_ALLOC + RC_ALLOC + boxed ADT + interp + 3 closure env + parallel-for + 1 doc cross-ref) |
| src/lir/emit_web.c | 1 (FrameState malloc guard) |

Total across src/: **31** iron_oom_abort references. Every audit-listed H-severity allocation site in Phase 65 ranks 1-4 plus Wasm-W1 is covered.

## Decisions Made

1. **iron_oom_abort in src/runtime/iron_oom.c, not diagnostics.c** — diagnostics.c drags parser/ast.h + stb_ds which iron_runtime tests must not link. Keeping the definition in a dedicated runtime .c file preserves iron_runtime's self-contained property and mirrors the 59-01c build-path parity convention. The declaration still lives adjacent to iron_ice in diagnostics.h so call-sites use a unified header.
2. **Named realloc temporary + commit-after-check pattern** — `_push/_put/_add` capture realloc into a named local (`new_items`, `new_keys`, `new_values`) and only commit to `self->` after the NULL check passes. This leaves the original backing pointer intact on OOM, so the abort fires with zero side effects on the live collection — no half-migrated state visible to any signal handler or core-dump reader.
3. **Integer-overflow guard before the realloc call** — `new_cap < self->capacity` after the doubling catches the int64_t wraparound case before we feed `(size_t)-4` to realloc (which on glibc maps to `SIZE_MAX / 2 - ε` and would either succeed with a mind-bogglingly large allocation or return NULL with no diagnosis). Aborting with a "capacity overflow" suffix in the location literal makes the overflow bisectable.
4. **Location literals identify structural call-sites, not source lines** — generated C cannot meaningfully use `__FILE__` because that resolves to the user's compiled .c file. Structural literals like `"emit_c HEAP_ALLOC"`, `"emit_c closure env (A)"`, `"Iron_List_int64_t_push: capacity overflow"` let stderr grep bisect which emitter path or collection method faulted, without needing a debugger or source-map.
5. **Unit test strategy: capacity overflow for LIST, create_with_capacity for MAP** — forcing realloc to return NULL portably requires LD_PRELOAD or link-time `__wrap_realloc`, neither of which the existing Unity harness supports. For LIST the capacity-overflow arm hits the same iron_oom_abort with no prerequisites. For MAP the _put macro does a linear scan over self->keys[0..count] before the capacity check, so the overflow-capacity trick segfaults on the scan — instead we exercise _create_with_capacity(INT64_MAX/4) which has no scan prerequisite and fails malloc directly.
6. **Zero additional include wiring in emitters** — emit_c.c and emit_web.c already emit `#include "runtime/iron_runtime.h"` into the generated C prelude, and iron_runtime.h transitively includes diagnostics/diagnostics.h after Task 1. So every compiled binary resolves iron_oom_abort for free — no emit_c.c or emit_web.c prelude edits needed in Task 3.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] iron_oom_abort definition location — diagnostics.c would drag parser/ast.h into iron_runtime tests**

- **Found during:** Task 1 (previous executor, committed in `97a686e`)
- **Issue:** The plan specified placing the iron_oom_abort definition in `src/diagnostics/diagnostics.c`. That file also contains `iron_ice` and `iron_node_assert_kind_impl`, which pull in parser/ast.h + stb_ds transitively. iron_runtime is a self-contained static library that unit tests link against WITHOUT parser/ast.h, so adding diagnostics.c to iron_runtime's source list would have dragged the entire AST layer into every runtime unit test binary.
- **Fix:** Created a new `src/runtime/iron_oom.c` with just the iron_oom_abort definition. The declaration still lives in diagnostics.h adjacent to iron_ice (so call-sites use a unified header) but the definition is in the runtime tree where iron_runtime's build rules can see it without pulling diagnostics.c. The plan's own `<include_path_notes>` section actually documented this as the fallback and the previous executor took it.
- **Files modified:** src/runtime/iron_oom.c (created), CMakeLists.txt (iron_runtime target), src/cli/build.c (ironc build-path parity — build_src_list / free_src_list / invoke_clang argv_buf lanes both windows and unix).
- **Verification:** `nm libiron_runtime.a | grep iron_oom_abort` shows the symbol exported; ctest 74/74 green (was 72/72 at end of 67-01, +2 new OOM unit tests from Task 2).
- **Committed in:** 97a686e (Task 1 commit).

**2. [Rule 3 - Blocking] Task 3 new_items = (T *)realloc appears twice, not once (acceptance criterion was overstrict)**

- **Found during:** Task 2 acceptance check (current execution).
- **Issue:** Plan acceptance criterion said `grep -c "new_items = (T \*)realloc" src/runtime/iron_runtime.h` must return exactly 1. It actually returns 2 because IRON_SET_IMPL `_add` was written using the same local variable name and `(T *)` cast as IRON_LIST_IMPL `_push` — both macros take a type parameter named `T`. This is semantically correct (the SET `_add` path needs the same named-temp-then-commit pattern) but fails the literal grep count.
- **Fix:** No code change — the duplicate grep hit is correct. The acceptance criterion was stricter than necessary; keeping IRON_SET_IMPL consistent with IRON_LIST_IMPL is preferable to artificially renaming the temporary just to satisfy an exact-count grep.
- **Files modified:** None (deviation is in the acceptance criterion interpretation, not the code).
- **Verification:** ctest 74/74 green including both new OOM unit tests; the SET _add path is a direct mirror of LIST _push with the same safety invariants.
- **Committed in:** 61f0a8c (Task 2 commit).

**3. [Rule 2 - Missing Critical] 67-AUDIT-STATUS.md audit rows not automatically marked DONE after fix lands**

- **Found during:** Task 3 completion.
- **Issue:** The plan's `<success_criteria>` says "Audit ranks 1, 2, 3, 4 move from OPEN to DONE in 67-AUDIT-STATUS.md (updated in summary commit)" but the per-task commits for Tasks 2 + 3 did not update the audit status doc — the doc update was implicitly deferred to the plan-metadata commit. The AUDIT-STATUS doc is a grep-verifiable source of truth for downstream plans (67-03..67-08 all read it first), so letting it drift out of sync with head even for one commit is a correctness risk.
- **Fix:** Updated `.planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` — ranks 1-4 + Wasm-W1 moved from OPEN to DONE with commit hashes + evidence columns. Summary line updated from "13 DONE / 7 OPEN" to "17 DONE / 3 OPEN". New H-severity findings table updated to add a Status column. Plan Assignment for OPEN Rows table updated with Status column noting which 67-02 rows are DONE and which 67-03 rows remain OPEN.
- **Files modified:** .planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md.
- **Verification:** `grep -c "DONE — Phase 67-02" .planning/phases/67-correctness-fixes-+-crash-canaries/67-AUDIT-STATUS.md` returns ≥ 5 (ranks 1, 2, 3, 4, Wasm-W1).
- **Committed in:** Plan metadata commit (below) alongside SUMMARY + STATE.md + ROADMAP.md + REQUIREMENTS.md.

---

**Total deviations:** 3 auto-fixed (1 blocking per-executor, 1 blocking acceptance-criterion, 1 missing critical audit-doc sync)
**Impact on plan:** All three deviations were infrastructural — none changed the fix semantics. Deviation 1 was forced by build-graph constraints and actually improved the outcome (iron_runtime stays self-contained). Deviation 2 was an acceptance-criterion overstrictness and the correct resolution was to keep the code consistent. Deviation 3 closes a doc-staleness risk that would have affected 67-03..67-08 readers. No scope creep.

## Issues Encountered

None - all three tasks executed with no test regressions, no build failures, and no investigation loops. The previous executor's mid-plan API 500 crash left Task 2 on disk in a complete state (16 iron_oom_abort call-sites + 2 unit tests + CMakeLists wiring) that passed build + ctest on first run when the continuation executor verified it. Task 3 was implemented from scratch against a clean emit_c.c + emit_web.c.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- **FIX-01 closed.** All four H-severity ranked allocation failures + Wasm-W1 are fixed with iron_oom_abort guards. Phase 67 audit status: 17 DONE / 3 OPEN (was 13 DONE / 7 OPEN).
- **FIX-02 foundation landed.** The iron_oom_abort helper is the foundation every FIX-02 OOM guard in 67-04/67-05/67-06 will consume. Those plans can now `grep -r iron_oom_abort src/` to see the canonical usage pattern and follow the named-temp-then-commit convention established in IRON_LIST/MAP/SET.
- **67-03 unblocked.** FIX-04 integer-safety tail (ranks 14, 15, 19) + FIX-04 enum-switch tail can proceed independently — they do not touch allocation paths and the iron_oom_abort helper is stable.
- **67-08 unblocked.** REG-02 crash-canary fixtures will exercise these new guards; 67-02 landed 2 canary-style fixtures already (null_heap_alloc_malloc / null_rc_alloc_malloc) that the 67-08 plan can use as template references.
- **Integration suite grew 352 → 354.** ctest 74/74 green (was 72/72 at end of 66-05 + 67-01; +2 new OOM unit tests from this plan).
- **No blockers, no concerns.** The plan executed autonomously despite the mid-plan API 500 crash of the previous executor — the continuation executor inspected the uncommitted state, confirmed Task 2 was complete on disk, committed it, then executed Task 3 from scratch without re-doing any prior work.

## Self-Check: PASSED

All claimed files exist on disk and all claimed commits are present in git history:

- `src/runtime/iron_oom.c` ✓
- `tests/unit/test_alloc_list_push_oom.c` ✓
- `tests/unit/test_alloc_map_put_oom.c` ✓
- `tests/integration/null_heap_alloc_malloc.iron` + `.expected` ✓
- `tests/integration/null_rc_alloc_malloc.iron` + `.expected` ✓
- commit `97a686e` (Task 1) ✓
- commit `61f0a8c` (Task 2) ✓
- commit `b2e1555` (Task 3) ✓

Grep census verified: 31 iron_oom_abort references across src/ (3 in diagnostics.h + 2 in iron_oom.c + 16 in iron_runtime.h + 9 in emit_c.c + 1 in emit_web.c). End-to-end verification: `ironc build --debug-build` on both new fixtures produces `.iron-build/main.c` containing the literal `if (!_v4) iron_oom_abort("emit_c HEAP_ALLOC");` / `"emit_c RC_ALLOC"` guards. ctest 74/74 green. Integration suite 354/354 green (was 352 at end of 66-05 + 67-01; +2 new fixtures from this plan).

---
*Phase: 67-correctness-fixes-+-crash-canaries*
*Completed: 2026-04-13*
