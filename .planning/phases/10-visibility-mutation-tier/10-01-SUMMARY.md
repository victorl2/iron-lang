---
phase: 10-visibility-mutation-tier
plan: 01
subsystem: lsp-nav
tags: [vis, predicate, stdlib-carveout, symbol-identity, drift-guard, lsp, c17]

# Dependency graph
requires:
  - phase: 09-ast-analyzer-surface
    provides: Phase 9 D-08 explicitly deferred is_pub semantic mapping to Phase 10; 09-01 walker verify + 09-03 hover modifier prefix machinery
  - phase: 03-m2-navigation-understanding
    provides: NAV-15 sealed-tree invariant + NAV-16 walker shape + ilsp_symbol_id_derive identity triple (FNV-1a)
provides:
  - "ilsp_vis_is_public(decl_node) -- LSP-only visibility predicate normalizing the v3 AST non-uniformity (Iron_Field.is_pub vs Iron_FuncDecl/MethodDecl.is_private vs ObjectDecl/InterfaceDecl/EnumDecl/ValDecl/VarDecl no-axis) into a single positive boolean"
  - "ilsp_vis_can_see(decl_canonical, requester_canonical, decl_node) -- cross-module gate: NULL default-allow -> pointer-eq fast path -> strcmp -> stdlib carve-out (XXX_PHASE_14 MIG-01) -> visibility predicate"
  - "ilsp_nav_path_is_stdlib(canonical_path) -- promoted from rename/apply.c local static to nav_common public symbol (D-08 lift) so visibility.c shares the same definition"
  - "tests/lsp/unit/test_v3_visibility_predicate.c -- 10 RUN_TESTs (NULL safety, same-module short-circuit, stdlib carve-out, every decl-kind arm, determinism gate)"
  - "tests/lsp/unit/test_v3_visibility_symbol_identity_drift.c -- D-12 invariant: pub keyword toggle MUST NOT change FNV-1a hash, name_path, canonical_path, or kind on the symbol identity triple"
affects: [10-02-vis-fan-out, 10-03-tier-hover, 11-patch, 14-mig]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Single-helper-many-call-sites (Phase 3 nav_common.c pattern continued): visibility.c is the next instance, exporting two pure functions consumed by ≥7 nav-facade call sites in 10-02 + 10-03"
    - "LSP-only adapter at compiler-facing boundary: Phase 10 normalizes AST non-uniformity (positive `is_pub` on fields, v2-inverse `is_private` on funcs/methods, no-axis on objects/interfaces/enums/vals/vars) WITHOUT touching the parser/analyzer/HIR/LIR. Compiler-side AST stays unchanged"
    - "RESEARCH conflict resolution at the API boundary: Conflict 1 dropped non-existent `is_pub_setter` (Iron_Field has no such field); Conflict 3 reduced 8-arm switch to 3-arm + default-true (parser drops `private` keyword on top-level non-func/non-method/non-field decls per parser.c:4047, 4606, 4737)"
    - "XXX_PHASE_N marker discipline (Phase 8 D-04): XXX_PHASE_14 MIG-01 marker emitted at the stdlib carve-out site so Phase 14 stdlib `pub` migration finds it"

key-files:
  created:
    - "src/lsp/facade/nav/visibility.h"
    - "src/lsp/facade/nav/visibility.c"
    - "tests/lsp/unit/test_v3_visibility_predicate.c"
    - "tests/lsp/unit/test_v3_visibility_symbol_identity_drift.c"
    - ".planning/phases/10-visibility-mutation-tier/deferred-items.md"
  modified:
    - "src/lsp/facade/nav/nav_common.h (+ ilsp_nav_path_is_stdlib decl)"
    - "src/lsp/facade/nav/nav_common.c (+ ilsp_nav_path_is_stdlib body, byte-identical to ex-apply.c original)"
    - "src/lsp/facade/edit/rename/apply.c (- static path_is_stdlib; call site updated to ilsp_nav_path_is_stdlib)"
    - "src/lsp/facade/edit/complete/buckets.c (- dormant decl_is_private #if 0 sketch)"
    - "CMakeLists.txt (+ src/lsp/facade/nav/visibility.c on the ironls executable)"
    - "tests/lsp/unit/CMakeLists.txt (+ visibility.c on _LSP_PHASE3_NAV_FACADE_SRC and the 2 new test executables)"

key-decisions:
  - "RESEARCH Conflict 1 resolved: ilsp_vis_is_public_setter DROPPED from the API. Iron_Field has no is_pub_setter field; that bit lives on Iron_AssignStmt. Hover renders pub var/pub val from (is_pub, is_var) directly in 10-03; rename uses read-axis only per D-09."
  - "RESEARCH Conflict 3 resolved: 3-arm switch (FIELD, FUNC_DECL, METHOD_DECL) + default-true. The 8-arm sketch in CONTEXT.md D-02 included ObjectDecl/InterfaceDecl/EnumDecl/ValDecl/VarDecl arms which would be dead code -- those decls have NO is_private bit (parser drops the keyword at 4047/4606/4737)."
  - "Stdlib carve-out implemented via promoted `ilsp_nav_path_is_stdlib` (D-08 lift); XXX_PHASE_14 MIG-01 marker placed at the carve-out branch so Phase 14 corpus migration finds the flip site."
  - "D-13 per-request requester path threading: ilsp_vis_can_see takes both paths as parameters (no globals) so concurrent request threads share no mutable state. Pointer-equality fast path falls out of strcmp (Phase 3 NAV-15 arena interning convention)."
  - "D-15 abort-audit posture preserved: NULL inputs return defensively (false for is_public, true for can_see same-module-without-canonical graceful fallback). Zero new iron_ice / iron_oom_abort sites added (HARD-04 invariant intact)."

patterns-established:
  - "Pattern: anonymous typedef struct field access in tests. Iron_Field/FuncDecl/MethodDecl/ObjectDecl have no `base` member; Iron_Span span + Iron_NodeKind kind are laid out inline. Tests stack-allocate the concrete decl, set `kind` directly, and cast to const Iron_Node * at the call site. Documented inline in test_v3_visibility_predicate.c header."
  - "Pattern: -Werror=switch-enum compatibility for partial-enum switches. Cast switch operand to (int) and use explicit `default:` arm (matches buckets.c:104, workspace_symbol.c:62). Future facade switches over Iron_NodeKind for partial coverage SHOULD follow this convention to avoid -Wswitch-enum noise."

requirements-completed: []  # VIS-01..04 are PROVIDES-IN-PROGRESS for Phase 10 (closed by Plan 10-02 fan-out). Plan 10-01 ships only the predicate prerequisite + drift-guard; no requirements close in this plan.

# Metrics
duration: 10min
completed: 2026-04-28
---

# Phase 10 Plan 01: Foundations Summary

**Phase 10 foundations: visibility predicate + stdlib carve-out lift + D-12 drift-guard**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-04-28T01:20:54Z
- **Completed:** 2026-04-28T01:30:34Z
- **Tasks:** 4 (each committed atomically)
- **Files modified:** 9 (5 created, 4 modified — 1 of the modifications is a deletion-only change in buckets.c)

## Accomplishments

- Shipped `src/lsp/facade/nav/visibility.{c,h}` (~110 LoC total) — the single LSP-only adapter that normalizes the v3 AST visibility non-uniformity into a positive boolean predicate. Two functions: `ilsp_vis_is_public(decl_node)` (3-arm switch + default-true) and `ilsp_vis_can_see(decl_canonical, requester_canonical, decl_node)` (cross-module gate with stdlib carve-out).
- Promoted `path_is_stdlib` from a rename-internal static helper to a public `ilsp_nav_path_is_stdlib` symbol in `nav_common.{c,h}` (D-08 lift). One definition shared by visibility.c + apply.c; eliminated a future drift surface.
- Deleted the dormant `decl_is_private` `#if 0` block in `buckets.c` (Phase 4 sketch reserved for an auto-import privacy filter that never wired). Visibility logic now lives in exactly one place.
- Wrote a 10-RUN_TEST predicate Unity test asserting NULL safety, same-module short-circuit (pointer-eq + strcmp), stdlib carve-out (D-08), every decl-kind arm (Field public/private, FuncDecl public/private, MethodDecl private, ObjectDecl default-true), and a 1000-iteration determinism gate (Validation § Determinism Gate).
- Wrote the D-12 drift-guard Unity test asserting that toggling `Iron_FuncDecl.is_private` (the v3 representation of `pub` vs no-`pub`) leaves the FNV-1a hash, name_path, canonical_path, and kind of the symbol identity triple byte-identical.
- Resolved RESEARCH Conflict 1 (drop `ilsp_vis_is_public_setter`) and RESEARCH Conflict 3 (3-arm switch, not 8-arm) at the predicate API surface. Both conflict resolutions documented inline in visibility.h + visibility.c source comments and in the test file headers.

## Task Commits

Each task was committed atomically:

1. **Task 1: Wave 0 — TEST_IGNORE'd test stubs + CMake registration** — `d0b2d12` (test)
2. **Task 2: visibility.{c,h} + ilsp_nav_path_is_stdlib lift** — `226ec1a` (feat)
3. **Task 3: Delete dormant decl_is_private #if 0 sketch** — `48696b4` (chore)
4. **Task 4: Flip Wave 0 stubs to real Unity assertions (predicate + D-12 drift-guard)** — `149de6b` (test)

## Files Created/Modified

### Created
- `src/lsp/facade/nav/visibility.h` (60 lines) — exports `ilsp_vis_is_public` + `ilsp_vis_can_see` with full docstring covering D-01/D-02/D-04/D-08/D-13 contracts.
- `src/lsp/facade/nav/visibility.c` (62 lines) — implementation. 3-arm switch + default-true; cross-module gate with NULL default-allow + pointer-eq fast path + strcmp + stdlib carve-out + visibility fall-through.
- `tests/lsp/unit/test_v3_visibility_predicate.c` (135 lines) — 10 Unity test bodies including determinism gate.
- `tests/lsp/unit/test_v3_visibility_symbol_identity_drift.c` (90 lines) — D-12 hash equality across pub-toggle.
- `.planning/phases/10-visibility-mutation-tier/deferred-items.md` — pre-existing build failures observed during execution but NOT introduced by Phase 10 (test_ast_sealed -Werror=address regression in the IRON_AST_ASSERT_UNSEALED macro; test_string_intern_race missing libtsan host library; test_v3_symbol_id_corpus baseline path mismatch in worktree).

### Modified
- `src/lsp/facade/nav/nav_common.h` — appended `ilsp_nav_path_is_stdlib` declaration after the existing path/uri helper block.
- `src/lsp/facade/nav/nav_common.c` — appended `ilsp_nav_path_is_stdlib` body (byte-identical to the ex-apply.c original).
- `src/lsp/facade/edit/rename/apply.c` — deleted local `static bool path_is_stdlib`; updated call site at line 452 to `ilsp_nav_path_is_stdlib`. The path_is_dep neighbour stays local.
- `src/lsp/facade/edit/complete/buckets.c` — deleted the 13-line `#if 0`/`#endif` block + leading comment.
- `CMakeLists.txt` — added `src/lsp/facade/nav/visibility.c` to the `ironls` executable source list.
- `tests/lsp/unit/CMakeLists.txt` — added 2 new test_executables (predicate + drift-guard) with explicit source lists; added `visibility.c` to `_LSP_PHASE3_NAV_FACADE_SRC` so every Plan 10-02 facade test that links the shared NAV stack picks up the predicate transparently.

## Decisions Made

(See `key-decisions:` in frontmatter for the structured list.)

Two RESEARCH-conflict resolutions made at the API surface (Conflict 1 + Conflict 3) and one marker-discipline decision (XXX_PHASE_14 placement) define the contract for Plans 10-02 + 10-03. Plan 10-02 will consume `ilsp_vis_can_see` from 7 nav-facade call sites; Plan 10-03 will consume `ilsp_vis_is_public` from hover/completion/signature-help renderers. Both downstream plans inherit the abort-audit posture (Pitfall 5 default-allow) and the determinism gate.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Fixed nested `/*` in visibility.c XXX_PHASE_14 comment**

- **Found during:** Task 2 (visibility.c first build)
- **Issue:** Plan source quoted the comment text as ``stdlib `pub` onto src/stdlib/*.iron.``. The `*.iron` is interpreted by gcc's `-Werror=comment` as the start of a nested C comment (`/*`), failing the build.
- **Fix:** Reworded the comment to drop the leading `/*` glob fragment: ``stdlib `pub` onto the stdlib .iron surface (src/stdlib + src/runtime).``
- **Files modified:** `src/lsp/facade/nav/visibility.c`
- **Verification:** `cmake --build build --target ironls` succeeds; `grep -c "XXX_PHASE_14 MIG-01:" src/lsp/facade/nav/visibility.c` returns 1.
- **Committed in:** `226ec1a` (Task 2 commit)

**2. [Rule 3 - Blocking] Added line_index.c + utf.c + yyjson.c to test source lists**

- **Found during:** Task 2 (link of test_v3_visibility_symbol_identity_drift)
- **Issue:** Adding `nav_common.c` to the predicate-test sources brought in a transitive dep on `ilsp_byte_of_line` (defined in `src/lsp/store/line_index.c`) which the standalone test source list did not include. Plan called for `nav_common.c` as a direct dep of the test (predicate test compiles standalone) but did not enumerate its transitive deps.
- **Fix:** Added `src/lsp/store/line_index.c` + `src/lsp/store/utf.c` + `src/vendor/yyjson/yyjson.c` to both new test_executable source lists. Added `iron_compiler iron_runtime` link libraries (predicate test) and `iron_compiler iron_runtime iron_stdlib` (drift-guard test, since symbol_id.c references stdlib types via Iron_Symbol).
- **Files modified:** `tests/lsp/unit/CMakeLists.txt`
- **Verification:** Both test executables link clean and run to PASS / IGNORED in Wave 0; PASS in Wave 1.
- **Committed in:** `226ec1a` (Task 2 commit)

**3. [Rule 1 - Plan Bug] Corrected field-name references in test bodies**

- **Found during:** Task 4 (writing the actual Unity assertions)
- **Issue:** Plan source used `fd.base.kind`, `f.base.kind`, `s.kind`, `IRON_SYMBOL_FUNC` and 3-arg `ilsp_symbol_id_derive(sym, canonical, arena)`. Verified against `src/parser/ast.h` + `src/analyzer/scope.h` + `src/lsp/facade/nav/symbol_id.h`:
  - `Iron_Field`/`Iron_FuncDecl`/`Iron_MethodDecl`/`Iron_ObjectDecl` are anonymous typedef structs whose first two fields are `{ Iron_Span span; Iron_NodeKind kind; }`. There is no `base` member.
  - `Iron_Symbol` uses `sym_kind` (typed `Iron_SymbolKind`), not `kind`.
  - The function symbol kind is `IRON_SYM_FUNCTION`, not `IRON_SYMBOL_FUNC`.
  - `ilsp_symbol_id_derive` takes 4 args: `(sym, canonical_path, program, arena)` — the program parameter (NULL-safe for top-level decls per Phase 9 D-04) was missing from the plan-quoted call.
- **Fix:** Used `fd.kind` / `f.kind` / `od.kind` directly; used `s.sym_kind = IRON_SYM_FUNCTION`; passed `program=NULL` (top-level FuncDecl falls back to "<module>.<name>" name_path encoding).
- **Files modified:** `tests/lsp/unit/test_v3_visibility_predicate.c`, `tests/lsp/unit/test_v3_visibility_symbol_identity_drift.c`
- **Verification:** Both tests compile + PASS. Inline header comments in both test files document the field-naming for future maintainers.
- **Committed in:** `149de6b` (Task 4 commit)

---

**Total deviations:** 3 auto-fixed (1 blocking compile, 1 blocking link, 1 plan-bug field-name correction)
**Impact on plan:** All three were mechanical corrections to plan-quoted text that did not match the v3 AST surface (verified post-Phase-9 rebase). No semantic change to the plan; the predicate behavior, test coverage, and acceptance criteria remained identical to the plan spec.

## Issues Encountered

Three pre-existing build failures observed during the full-build sweep, **NOT** introduced by Phase 10 work. All three documented in `.planning/phases/10-visibility-mutation-tier/deferred-items.md` and verified to fail on the bare worktree base via `git stash`:

1. `tests/lsp/unit/test_ast_sealed.c:81` — `-Werror=address` on `IRON_AST_ASSERT_UNSEALED(&p)` (gcc 11.5 raises that the address of a stack-local is always non-NULL; the macro's defensive NULL guard is now redundant given how callers invoke it).
2. `tests/unit/test_string_intern_race` — link error: `cannot find /usr/lib64/libtsan.so.0.0.0`. Host environment missing the TSAN runtime; not a code regression.
3. `tests/lsp/unit/test_v3_symbol_id_corpus` — `test_v2_zero_churn` baseline divergence because the worktree resolves `IRON_SOURCE_TREE_ROOT` to `/home/victor/code/iron-lsp/iron-lang/.claude/worktrees/...` but the Phase 9 baseline was generated against the parent canonical path. Worktree-only failure; primary tree is unaffected.

The plan-exit gate `phase-m2-invariant` shows 21/23 tests passing with the 2 known pre-existing failures listed above. The phase-m2-invariant subset that is RELEVANT to Plan 10-01 (visibility predicate + drift-guard + parity gate) is 100% green.

## User Setup Required

None — no external service configuration required. Plan 10-01 ships entirely inside `src/lsp/` and `tests/lsp/`.

## Next Phase Readiness

- Plan 10-02 (VIS fan-out — references / workspace_symbol / definition / declaration / typeDefinition / implementation / rename) can run in parallel with Plan 10-03. Both consume the predicate from this plan.
- Plan 10-03 (TIER + VIS-05 hover extension + closeout) only needs the predicate from this plan; its files_modified set is disjoint from 10-02.
- The `XXX_PHASE_14 MIG-01` marker in `visibility.c:55-58` is greppable for the Phase 14 stdlib `pub` migration to find the carve-out flip site.
- The `ilsp_nav_path_is_stdlib` symbol is now public — Plan 11 (PATCH) can reuse it for cross-module patch visibility checks per Phase 11 PATCH-05.

## Self-Check: PASSED

All claimed files exist and all four task commits are present in `git log`:

- `src/lsp/facade/nav/visibility.h` — FOUND
- `src/lsp/facade/nav/visibility.c` — FOUND
- `tests/lsp/unit/test_v3_visibility_predicate.c` — FOUND
- `tests/lsp/unit/test_v3_visibility_symbol_identity_drift.c` — FOUND
- `.planning/phases/10-visibility-mutation-tier/deferred-items.md` — FOUND
- Commit `d0b2d12` (Task 1) — FOUND
- Commit `226ec1a` (Task 2) — FOUND
- Commit `48696b4` (Task 3) — FOUND
- Commit `149de6b` (Task 4) — FOUND
- Plan-exit grep gates: 7/7 PASS (`static bool path_is_stdlib`=0 in apply.c; `decl_is_private`=0 in buckets.c; `is_pub_setter`=0 in visibility.h; `ilsp_vis_is_public`=3 in visibility.h; `ilsp_vis_can_see`=2 in visibility.h; `XXX_PHASE_14`=1 in visibility.c; `ilsp_nav_path_is_stdlib`=1 in nav_common.h).
- Plan-exit ctest gates: parity green; phase-10-specific tests green. The two pre-existing phase-m2-invariant failures (test_ast_sealed, test_v3_symbol_id_corpus) verified to fail on bare base commit and documented in deferred-items.md.

---
*Phase: 10-visibility-mutation-tier*
*Completed: 2026-04-28*
