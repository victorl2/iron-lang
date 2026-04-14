---
phase: 66-structural-protections-linux-release-ci
plan: 01
subsystem: compiler-infrastructure
tags: [ast, static-assert, debug-assertions, diagnostics, prot-01, prot-03]

# Dependency graph
requires:
  - phase: 65-correctness-audit
    provides: CORRECTNESS-AUDIT.md rank 9 finding — Iron_ExprNode layout assumption in hir_lower.c
provides:
  - Iron_ExprNode typedef published as single-source-of-truth in src/parser/ast.h
  - 23 expression AST types locked to the {span, kind, resolved_type} prefix via 5 _Static_asserts each (115 compile-time checks total)
  - iron_ice noreturn helper declared in diagnostics.h and implemented in diagnostics.c as the canonical internal-compiler-error abort path
  - IRON_NODE_ASSERT_KIND(node, kind) macro in ast.h — Debug-build kind check routed through iron_ice, zero-cost ((void)0) in Release (NDEBUG)
  - iron_node_assert_kind_impl(node, expected, file, line, func) backing function in diagnostics.c with NULL-safe dispatch
affects:
  - 66-02-plan (-Werror=switch-enum rollout)
  - 66-03-plan (M/L severity IRON_NODE_ASSERT_KIND walkthrough — every cast site gains the macro)
  - 66-04-plan (H severity blind-cast rewrites — real struct from ast.h + IRON_NODE_ASSERT_KIND guard)
  - Phase 67 REG-02 crash canaries (will use the same macro at new cast sites)

# Tech tracking
tech-stack:
  added:
    - "<stddef.h> in ast.h (for offsetof in _Static_assert block)"
    - "<stdarg.h> in diagnostics.c (for va_list in iron_ice)"
  patterns:
    - "Macro-per-type _Static_assert via IRON_ASSERT_EXPR_PREFIX(T) — one invocation per expression type, 5 assertions each, #undef'd at end of block"
    - "Debug/Release-dual macro gated on #ifndef NDEBUG — real call in Debug, ((void)0) in Release"
    - "iron_ice as the canonical ICE abort path, distinct from iron_diag_emit (user-facing diagnostics)"
    - "Integer kind values in ICE messages instead of stringified names (keeps impl dep-free; future phase can add iron_node_kind_name helper)"

key-files:
  created: []
  modified:
    - "src/parser/ast.h — +72 lines: Iron_ExprNode typedef, IRON_NODE_ASSERT_KIND macro + fwd decl, IRON_ASSERT_EXPR_PREFIX block at end of file"
    - "src/diagnostics/diagnostics.h — +28 lines: iron_ice noreturn declaration"
    - "src/diagnostics/diagnostics.c — +40 lines: iron_ice implementation + iron_node_assert_kind_impl implementation, new #includes <stdarg.h> and parser/ast.h"
    - "src/hir/hir_lower.c — -7 lines: local Iron_ExprNode typedef removed, expr_type() unchanged and now resolves against ast.h shared typedef"

key-decisions:
  - "[Phase 66-01] Iron_ExprNode uses a named struct tag (typedef struct Iron_ExprNode) not anonymous, so forward references and pointer types can refer to the tag"
  - "[Phase 66-01] IRON_ASSERT_EXPR_PREFIX macro per-type with 5 asserts each (span/kind/resolved_type offsets + span/kind field sizes) — readable and catches both reordering and type-change mistakes"
  - "[Phase 66-01] Placed _Static_assert block at END of ast.h so all expression struct definitions are visible when the asserts run; typedef lives near the top so Iron_ExprNode is visible to downstream consumers"
  - "[Phase 66-01] iron_ice message uses integer kind values not stringified names — mapping Iron_NodeKind to strings would need a new helper or duplicate printer.c's mapping; integers are sufficient for bisection and keep the dep graph simple"
  - "[Phase 66-01] iron_node_assert_kind_impl declaration lives in ast.h (not diagnostics.h) so every AST-consuming TU that already includes ast.h gets the declaration for free without new transitive deps"
  - "[Phase 66-01] Task 3 (hir_lower.c local typedef removal) was rolled into Task 1 as a Rule 3 blocking-issue fix — the new ast.h typedef created a redefinition against the hir_lower.c local struct, so the local had to be deleted to unblock Task 1's build-verification step"

patterns-established:
  - "PROT-01 prefix enforcement template: define the prefix typedef once, then at end of header #define a per-type assertion macro, invoke it for every conforming type, #undef it. Future phases adding new expression kinds just add one line."
  - "PROT-03 macro/impl split: macro captures __FILE__/__LINE__/__func__ at the call site with zero boilerplate at the call, impl function handles NULL check + kind compare + iron_ice dispatch in one place"
  - "iron_ice format: 'iron: internal compiler error: <printf-formatted message>' — matches Rust compiler ICE output style; downstream CI grep patterns can rely on this prefix"

requirements-completed: [PROT-01, PROT-03]

# Metrics
duration: 17min
completed: 2026-04-13
---

# Phase 66 Plan 01: Structural Protections Foundation Summary

**Iron_ExprNode published in ast.h with 115 compile-time prefix asserts (23 types × 5 checks), plus iron_ice ICE helper and Debug-only IRON_NODE_ASSERT_KIND macro wired through the diagnostics layer.**

## Performance

- **Duration:** 17 min
- **Started:** 2026-04-13T00:00:27Z
- **Completed:** 2026-04-13T00:17:37Z
- **Tasks:** 3 (Task 3 subsumed into Task 1 per Rule 3)
- **Files modified:** 4

## Accomplishments

- `Iron_ExprNode` is now the single source of truth in `src/parser/ast.h` for the `{Iron_Span span; Iron_NodeKind kind; struct Iron_Type *resolved_type;}` prefix shared by every expression AST type. Previous location in `src/hir/hir_lower.c:101-105` is removed.
- 23 expression AST types (`Iron_IntLit` through `Iron_EnumConstruct`) are each locked to the prefix layout at compile time via `IRON_ASSERT_EXPR_PREFIX(T)` — 5 `_Static_assert` per type, 115 checks total. Adding a new expression type with a wrong field order now fails the build immediately instead of corrupting memory at runtime.
- `iron_ice(const char *fmt, ...)` declared in `diagnostics.h` (noreturn, printf-format) and implemented in `diagnostics.c` as the canonical internal-compiler-error path: prints `"iron: internal compiler error: <msg>"` to stderr, flushes, and calls `abort()`.
- `IRON_NODE_ASSERT_KIND(node, expected_kind)` macro in `ast.h` expands to `iron_node_assert_kind_impl(..., __FILE__, __LINE__, __func__)` in Debug and `((void)0)` in Release. The impl function NULL-checks the node, compares `node->kind` against `expected`, and routes mismatches through `iron_ice` with full call-site context.
- Full build + all 72 ctest unit tests + all 346 integration tests pass after the changes — no behavioral regression from removing the local `Iron_ExprNode` typedef in `hir_lower.c`.

## Task Commits

1. **Task 1: Move Iron_ExprNode to ast.h + add _Static_assert prefix enforcement** — `1ccbb6f` (feat)
   - Also includes the hir_lower.c local typedef removal that Task 3 was scheduled to do, folded in as a Rule 3 blocking-issue fix (see Deviations).
2. **Task 2: Create iron_ice helper + IRON_NODE_ASSERT_KIND macro + iron_node_assert_kind_impl** — `5cde0a5` (feat)
3. **Task 3: Remove local Iron_ExprNode from hir_lower.c and switch expr_type to the shared typedef** — subsumed into Task 1 commit `1ccbb6f`. No separate commit (implementation work already landed; Task 3 scope reduced to verification, which passed: full `ctest` + `run_integration.sh` green).

**Plan metadata:** will be in final commit below (SUMMARY.md + STATE.md + ROADMAP.md).

## Files Created/Modified

- `src/parser/ast.h` — Added `#include <stddef.h>`, `typedef struct Iron_ExprNode`, `IRON_NODE_ASSERT_KIND` macro + `iron_node_assert_kind_impl` forward decl, and the `IRON_ASSERT_EXPR_PREFIX(T)` macro block at end of file covering all 23 expression types.
- `src/diagnostics/diagnostics.h` — Added `iron_ice` declaration with `__attribute__((noreturn, format(printf, 1, 2)))` for GCC/Clang.
- `src/diagnostics/diagnostics.c` — Added `#include <stdarg.h>` and `#include "parser/ast.h"` at top, appended `iron_ice` and `iron_node_assert_kind_impl` implementations at end.
- `src/hir/hir_lower.c` — Removed the local 7-line `Iron_ExprNode` typedef block (lines 99-105); `expr_type()` function at lines 102-105 (post-edit) is untouched and now resolves `Iron_ExprNode` via the `parser/ast.h` include already present through `hir/hir_lower.h`.

## Decisions Made

All six decisions are recorded in the frontmatter `key-decisions` block above. Headline items:

1. **Macro-driven assert block over hand-rolled per-type asserts** — the `IRON_ASSERT_EXPR_PREFIX(T)` macro is defined once and invoked 23 times, `#undef`'d after. Adding a new expression type is a one-line change instead of a 5-line boilerplate block. The macro expansion at the preprocessor level still gives the compiler all 115 individual `_Static_assert` calls.
2. **`_Static_assert` block at END of ast.h** — the asserts reference field offsets of every concrete expression type (`Iron_IntLit`, `Iron_EnumConstruct`, …), so the typedefs for those must be visible when the asserts are processed. Placing the block at the end of the header is the simplest correct ordering.
3. **Integer kind values in ICE messages** — `iron_ice("expected kind %d, got %d", ...)` instead of mapping to string names. Keeps the impl function zero-dependency beyond `stdio`/`stdlib`; a future phase can add `iron_node_kind_name(Iron_NodeKind)` without breaking anything.
4. **Task 3 scope reduction** — since Task 1's verification requires hir_lower.c's local typedef gone (Rule 3), Task 3 becomes verification-only and does not produce a new commit. Documented explicitly so bisects and future plan counts are unambiguous.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Removed hir_lower.c local Iron_ExprNode typedef during Task 1 instead of Task 3**

- **Found during:** Task 1 (initial `cmake --build build` after adding `Iron_ExprNode` to `ast.h`)
- **Issue:** `src/hir/hir_lower.c:101-105` contained `typedef struct {...} Iron_ExprNode;` as a local unnamed struct. Publishing `typedef struct Iron_ExprNode {...} Iron_ExprNode;` in `ast.h` created a redefinition error because hir_lower.c's anonymous-struct typedef is a different type from ast.h's tagged-struct typedef in C. Build failed with:
  ```
  src/hir/hir_lower.c:105:3: error: typedef redefinition with different types
  ('struct Iron_ExprNode' (aka 'Iron_ExprNode') vs 'struct Iron_ExprNode')
  ```
- **Fix:** Deleted the 7-line local typedef block in `hir_lower.c` (the exact edit Task 3 would have performed). Left the `expr_type()` function untouched — it now resolves `Iron_ExprNode` via the shared typedef through the existing `parser/ast.h` include in `hir/hir_lower.h`.
- **Files modified:** `src/hir/hir_lower.c`
- **Verification:** `cmake --build build` now succeeds with 0 errors; `expr_type()` still compiles; `ctest` + `run_integration.sh` pass 100% afterwards.
- **Committed in:** `1ccbb6f` (Task 1 commit — folded in atomically so the tree is bisect-clean; committing Task 1's ast.h addition alone would have left HEAD broken).

**Task 3 consequence:** Task 3's implementation work is subsumed into `1ccbb6f`. Task 3's verification step (full `ctest` + `run_integration.sh`) was still executed — 72/72 unit tests and 346/346 integration tests pass. No separate Task 3 commit was created because git does not allow empty commits and all Task 3 code changes already shipped in Task 1.

---

**Total deviations:** 1 auto-fixed (1 blocking-issue)
**Impact on plan:** Zero scope creep. The deviation only reorders when the hir_lower.c edit lands (Task 1 instead of Task 3); the total set of changes matches the plan's specification exactly. Bisectability is preserved — every commit in the sequence builds cleanly.

## Issues Encountered

- **Plan task ordering vs. atomic-commit requirement** — Task 1's acceptance criteria require a clean build, but Task 1's ast.h edit creates a redefinition with hir_lower.c's pre-existing local typedef. The plan sequenced the hir_lower.c cleanup into Task 3, which is impossible without breaking Task 1's verification. Resolved by rolling Task 3's hir_lower.c edit into Task 1 (Rule 3) and running Task 3's test suite as a verification step. This is documented for future plans: header-migration tasks need to land the source-side cleanup in the same commit.

- **`grep -c "_Static_assert" src/parser/ast.h` returns 6, not 115** — the plan's phase-level verification expects ≥100 literal `_Static_assert` occurrences, but all 5 `_Static_assert` lines live inside the `IRON_ASSERT_EXPR_PREFIX(T)` macro body; source-level grep counts 6 (1 comment + 5 in the macro definition). The compile-time behavior is still 115 checks (23 macro invocations × 5 asserts each) — verified by successful compilation with the layout-locking semantics intact. This is a grep-expression quirk, not a correctness issue. `grep -c "IRON_ASSERT_EXPR_PREFIX(" src/parser/ast.h` returns 24 (1 def + 23 invocations), which is the intended semantic check.

## User Setup Required

None - no external service configuration required. Pure internal compiler refactor.

## Next Phase Readiness

- **Plan 02 (-Werror=switch-enum rollout)** can proceed — `iron_ice` is now available for any switch that needs to emit an ICE in a `default:` branch. The `IRON_NODE_ASSERT_KIND` macro is available for the existing `default: iron_ice(...)` candidates.
- **Plan 03 (M/L severity IRON_NODE_ASSERT_KIND walkthrough)** is unblocked — `IRON_NODE_ASSERT_KIND` is live and zero-cost in Release, so sprinkling it at all 182 cast sites adds Debug-build safety without Release impact.
- **Plan 04 (H severity blind-cast rewrites)** is unblocked — blind-cast rewrites will use the public `Iron_ExprNode`/`Iron_ObjectDecl`/etc. types with `IRON_NODE_ASSERT_KIND` guards immediately preceding the cast.
- **No blockers introduced.** Every downstream plan in Phase 66 can consume the ast.h surface directly.

## Self-Check: PASSED

**Files verified:**
- FOUND: `src/parser/ast.h` — Iron_ExprNode typedef + IRON_ASSERT_EXPR_PREFIX block present (24 macro-name occurrences, `#include <stddef.h>` line present)
- FOUND: `src/diagnostics/diagnostics.h` — `void iron_ice` declaration present with noreturn attribute
- FOUND: `src/diagnostics/diagnostics.c` — `iron_ice` + `iron_node_assert_kind_impl` definitions present
- FOUND: `src/hir/hir_lower.c` — local Iron_ExprNode typedef removed (grep count 0); `Iron_ExprNode` still referenced by `expr_type()` via shared header

**Commits verified (git log --oneline):**
- FOUND: `1ccbb6f` — feat(66-01): move Iron_ExprNode to ast.h with _Static_assert prefix enforcement
- FOUND: `5cde0a5` — feat(66-01): add iron_ice helper and IRON_NODE_ASSERT_KIND macro

**Build verified:**
- FOUND: `cmake --build build` → 0 errors
- FOUND: `ctest --test-dir build --output-on-failure -j4 -E "benchmark"` → 72/72 tests passed
- FOUND: `tests/integration/run_integration.sh build/ironc` → 346/346 integration tests passed

---
*Phase: 66-structural-protections-linux-release-ci*
*Completed: 2026-04-13*
