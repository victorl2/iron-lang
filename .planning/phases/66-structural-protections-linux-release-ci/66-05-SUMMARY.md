---
phase: 66-structural-protections-linux-release-ci
plan: 05
subsystem: infra
tags: [audit, blind-cast, prot-03, defensive-asserts, lir, typecheck, resolve, iron_net, static_assert]

# Dependency graph
requires:
  - phase: 66-01
    provides: IRON_NODE_ASSERT_KIND macro + iron_ice noreturn helper in src/parser/ast.h and src/diagnostics/diagnostics.h
  - phase: 66-03
    provides: 9 H-severity PROT-04 rewrites + 1 M-severity demonstration site at escape.c, establishing the walkthrough pattern
provides:
  - IRON_NODE_ASSERT_KIND coverage at every AUDIT-01 M-severity row 10-23 site (24 enumerated rows + 5 unenumerated bonus siblings)
  - Loop-bound + non-NULL runtime asserts at every Iron_EnumVariant / Iron_Field cast from a void** array (rows 25, 26, 27, 29, 30, 33, 34 + 2 bonus siblings)
  - File-scope _Static_asserts on Iron_Field and Iron_TypeAnnotation in src/lir/emit_structs.c documenting the layout contract grep-visibly
  - _Static_assert(sizeof(load) <= sizeof(phi)) on the lir_optimize.c phi-to-load in-place rewrite (rows 31, 32)
  - Targeted fix at src/stdlib/iron_net.c:594: replaced const-stripping cast on iron_string_cstr with a local heap buffer + free pattern, eliminating silent SSO/heap corruption on Iron_tcpsocket_read (row 24)
affects:
  - Phase 67 (REG-02 crash-canary fixtures, FIX-01..04 correctness fixes) — every blind-cast site now aborts loudly in Debug
  - Phase 68 (libFuzzer targets) — Debug builds will surface wrong-kind cast bugs as ICE rather than silent UB

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Loop-bound + non-NULL assert pattern for sub-struct casts from void** arrays where IRON_NODE_ASSERT_KIND does not apply (Iron_EnumVariant, Iron_Field)"
    - "_Static_assert on union member size relationship to document and pin in-place rewrite invariants in lir_optimize.c"
    - "Local heap buffer + free pattern for stdlib functions that cannot meaningfully write into value-passed Iron_String arguments"

key-files:
  created: []
  modified:
    - src/analyzer/typecheck.c
    - src/analyzer/resolve.c
    - src/stdlib/iron_net.c
    - src/lir/emit_c.c
    - src/lir/emit_structs.c
    - src/lir/lir_optimize.c

key-decisions:
  - "Sub-struct casts (Iron_EnumVariant, Iron_Field) get loop-bound + non-NULL assert because they have no kind field; only true Iron_Node derivatives get IRON_NODE_ASSERT_KIND"
  - "Iron_TypeAnnotation IS an Iron_Node derivative (first field is Iron_NodeKind kind, IRON_NODE_TYPE_ANNOTATION enum value exists), so it gets IRON_NODE_ASSERT_KIND not just a runtime non-NULL assert"
  - "phi-to-load in-place rewrite at lir_optimize.c:187-188 is a single rewrite site covering both audit rows 31 and 32; consolidated into one _Static_assert + comment block instead of two separate asserts"
  - "iron_net.c row 24 fix uses a 64 KiB cap on the local recv buffer to avoid unbounded stack/heap pressure if a caller passes an oversized Iron_String capacity; the function is broken-by-design (Iron_String is value-typed and immutable) and the comment block makes that explicit"
  - "Unenumerated bonus rows in typecheck.c, emit_c.c, and emit_structs.c are covered as siblings of nearby enumerated rows because the audit's intent is structural (every cast of this shape) not literal (only the row in CORRECTNESS-AUDIT.md §1)"

patterns-established:
  - "Sub-struct array iteration assert: assert(idx >= 0 && idx < count); assert(array[idx] != NULL); CastT *p = (CastT *)array[idx];"
  - "TypeAnnotation cast: IRON_NODE_ASSERT_KIND(node, IRON_NODE_TYPE_ANNOTATION); Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)node;"
  - "Union member rewrite invariant: _Static_assert(sizeof(((Container *)0)->target_member) <= sizeof(((Container *)0)->source_member), \"in-place rewrite size fit\");"

requirements-completed: [PROT-03]

# Metrics
duration: ~55min
completed: 2026-04-13
---

# Phase 66 Plan 05: PROT-03 M-severity walkthrough Summary

**Mechanical IRON_NODE_ASSERT_KIND walkthrough across 25 enumerated AUDIT-01 M-severity blind-cast rows in typecheck.c, resolve.c, iron_net.c, emit_c.c, emit_structs.c, and lir_optimize.c, plus 5 unenumerated bonus siblings, completing PROT-03 coverage of every explicitly-flagged blind cast in the audit.**

## Performance

- **Duration:** ~55 min
- **Started:** 2026-04-13T02:33:53Z (continued from 66-03 ship)
- **Completed:** 2026-04-13T03:30:00Z
- **Tasks:** 3 atomic
- **Files modified:** 6 source files + this SUMMARY

## Accomplishments

- Every AUDIT-01 M-severity row explicitly enumerated in CORRECTNESS-AUDIT.md §1 (rows 10-34, excluding row 9 already handled by Plan 03 Task 3) has an IRON_NODE_ASSERT_KIND, loop-bound runtime assert, compile-time _Static_assert, or targeted fix applied.
- Cumulative PROT-03 coverage across Plan 03 + Plan 05: **29 IRON_NODE_ASSERT_KIND call sites** in src/ and **38 PROT-03/04 markers** total — both exceed the plan's minimum thresholds (27 ASSERT_KIND, 34 markers).
- iron_net.c:594 const-stripping cast — the single non-cast-shaped fix in this plan — is replaced with a local heap buffer + free pattern that eliminates silent corruption of value-passed Iron_String SSO/heap storage.
- 5 unenumerated bonus sibling rows in typecheck.c, emit_c.c, and emit_structs.c are covered alongside the enumerated rows to keep the audit intent ("every cast of this shape") rather than the audit literal ("only this row").

## Coverage Census — AUDIT-01 §1 M-severity rows 10-34

| Row | Audit File:Line | Post-merge File:Line | Pattern | Fix Applied | Description |
|----:|------------------|------------------------|---------|-------------|-------------|
| 10  | typecheck.c:443 | typecheck.c:459 | `(Iron_InterfaceDecl *)csym->decl_node` | NULL guard + ASSERT_KIND | type_satisfies_constraint: assert IRON_NODE_INTERFACE_DECL before cast in interface-constraint check |
| 11  | typecheck.c:503 | typecheck.c:519 | `(Iron_Ident *)generic_params[i]` | ASSERT_KIND | check_generic_constraints: assert IRON_NODE_IDENT in the constraint loop |
| 12  | typecheck.c:708 | typecheck.c:724 | `(Iron_Ident *)ed->generic_params[i]` | guarded ASSERT_KIND | mono-generic-enum scope build: assert IRON_NODE_IDENT |
| 13  | typecheck.c:1401 | typecheck.c:1443 | `(Iron_Ident *)od->generic_params[gi]` | guarded ASSERT_KIND | object-CONSTRUCT generic instantiation: assert IRON_NODE_IDENT |
| 14  | typecheck.c:1525 | typecheck.c:1567 | `(Iron_FuncDecl *)fn_sym->decl_node` | ASSERT_KIND | generic-call constraint check: assert IRON_NODE_FUNC_DECL before cast |
| 15  | typecheck.c:1531 | typecheck.c:1573 | `(Iron_Ident *)fd->generic_params[gi]` | guarded ASSERT_KIND | generic-call generic-param walk: assert IRON_NODE_IDENT |
| 16  | typecheck.c:2189 | typecheck.c:2247 | `(Iron_Ident *)ed->generic_params[gi]` | guarded ASSERT_KIND | enum-CONSTRUCT generic-param scope build: assert IRON_NODE_IDENT |
| 17  | typecheck.c:2210 | typecheck.c:2268 | `(Iron_Ident *)ed->generic_params[gi]` | guarded ASSERT_KIND | enum-CONSTRUCT generic-param mapping: assert IRON_NODE_IDENT |
| 18  | typecheck.c:2301 | typecheck.c:2291 | `(Iron_Ident *)ed->generic_params[gi]` | guarded ASSERT_KIND | enum-CONSTRUCT recursive generic mapping: assert IRON_NODE_IDENT |
| 18b (bonus) | — | typecheck.c:2359 | `(Iron_Ident *)ed->generic_params[gi]` | guarded ASSERT_KIND | enum-CONSTRUCT variant payload type substitution: unenumerated sibling, same pattern |
| 19  | typecheck.c:2771 | typecheck.c:2841 | `(Iron_IsExpr *)is_s->condition` | ASSERT_KIND | type-name narrowing branch: assert IRON_NODE_IS before cast |
| 20  | typecheck.c:3056 | typecheck.c:3126 | `(Iron_Block *)ss->body` | kind-guard + ASSERT_KIND | spawn-handle return-type walk: skip if body is not IRON_NODE_BLOCK then assert |
| 21  | typecheck.c:3225 | typecheck.c:3306 | `(Iron_InterfaceDecl *)iface_sym->decl_node` | NULL/kind guard + ASSERT_KIND | interface-completeness check: skip on wrong-kind decl, assert before cast |
| 22  | resolve.c:612 | resolve.c:641 | `(Iron_EnumDecl *)esym->decl_node` | NULL guard + ASSERT_KIND | enum-pattern variant validation: assert IRON_NODE_ENUM_DECL |
| 23  | resolve.c:689 | resolve.c:754 | `(Iron_EnumDecl *)esym->decl_node` | NULL guard + ASSERT_KIND | enum-construct variant validation: assert IRON_NODE_ENUM_DECL |
| 24  | iron_net.c:594 | iron_net.c:594 | `(uint8_t *)(uintptr_t)base` | targeted fix | Iron_tcpsocket_read: const-strip removed, recv into local heap buffer (64 KiB cap) + free, count/error tuple unchanged |
| 25  | emit_c.c:533 | emit_c.c:540 | `(Iron_EnumVariant *)adt_ed->variants[variant_idx]` | loop-bound assert | ADT enum-construct expression emitter |
| 26  | emit_c.c:3154 | emit_c.c:3232 | `(Iron_EnumVariant *)adt_ed->variants[variant_idx]` | loop-bound assert | ADT enum-construct statement emitter |
| 27  | emit_structs.c:67 | emit_structs.c:84 | `(Iron_Field *)od->fields[i]` | loop-bound assert + file-scope _Static_assert | ir_topo_visit field walker |
| 28  | emit_structs.c:69 | emit_structs.c:90 | `(Iron_TypeAnnotation *)f->type_ann` | ASSERT_KIND | TypeAnnotation IS Iron_Node-derived; assert IRON_NODE_TYPE_ANNOTATION |
| 29  | emit_structs.c:259 | emit_structs.c:281 | `(Iron_Field *)od->fields[i]` | loop-bound assert | struct-body emitter (emit_object_struct_body) |
| 29b | emit_structs.c:260 | emit_structs.c:287 | `(Iron_TypeAnnotation *)f->type_ann` | ASSERT_KIND | struct-body emitter type-annotation branch |
| 30  | emit_structs.c:315 | emit_structs.c:338 | `(Iron_Field *)od->fields[i]` | loop-bound assert + ASSERT_KIND on type_ann | emit_estimate_type_size |
| 31  | lir_optimize.c:109 | lir_optimize.c:187 | `phi->kind = IRON_LIR_LOAD` (in-place rewrite) | _Static_assert + comment | phi-to-load: sizeof(load) <= sizeof(phi) union-fit invariant |
| 32  | lir_optimize.c:186 | lir_optimize.c:188 | `phi->load.ptr = alloca_id` (same rewrite) | covered with row 31 | second line of the same rewrite — single _Static_assert covers both |
| 33  | lir_optimize.c:447 | lir_optimize.c:2136 | `(Iron_EnumVariant *)ced->variants[vi]` | loop-bound assert | inline-eligibility CONSTRUCT scan in optimize_inline_eligibility |
| 34  | emit_c.c:1407 | emit_c.c:1417 | `(Iron_EnumVariant *)ged->variants[vi]` | loop-bound assert | boxed-payload field-access deref check |

### Unenumerated bonus rows covered

| Bonus | File:Line | Pattern | Rationale |
|-------|------------|---------|-----------|
| Bonus #1 | typecheck.c:2359 | `(Iron_Ident *)ed->generic_params[gi]` | Sibling of rows 16-18; same generic-param scope build idiom in the variant payload type substitution loop |
| Bonus #2 | emit_c.c:4525 | `(Iron_EnumVariant *)aed->variants[avi]` | Sibling of rows 25/26/34; boxed-alloca scan loop-bound assert |
| Bonus #3 | emit_structs.c:606 | `(Iron_Field *)od57->fields[fi]` | Sibling of rows 27/29/30; Phase 57 split-collection from-Stor constructor field walker |

The audit's stated intent is "every cast of this shape" rather than "only this exact line". Bonus rows are flagged in the inline comments as `PROT-03 unenumerated bonus (AUDIT-01 M-severity sibling of row N)` so future grep can trace them back to the originating audit row.

### Acknowledged non-coverage

- The ~237 unenumerated L-severity bulk array-iteration casts across parser/analyzer/hir/lir/runtime/stdlib/cli (Iron_Param, Iron_MatchCase, miscellaneous EnumVariant/Field iterations not in the files this plan touched) remain protected only by the Plan 01 _Static_assert prefix enforcement and Plan 02 -Werror=switch-enum flag. They are NOT in Plan 05 scope. If Phase 67 finds they need explicit coverage, a follow-up walkthrough plan can extend the pattern from Plan 05.
- Several typecheck.c (Iron_FuncDecl *) casts at lines 480, 1680, 3315, 3452, 3506 are over different shapes (method_sigs[], params[], iface->method_sigs[], decl from Program decls list) and were not in the audit's row-14 scope. They use `node->kind == IRON_NODE_FUNC_DECL` checks upstream and were not retroactively rewritten in this plan; if Phase 67 audits them as part of FIX-01..04 coverage, the same ASSERT_KIND pattern applies.

## Task Commits

Each task was committed atomically:

1. **Task 1: typecheck.c M-severity walkthrough (rows 10-21 + 1 bonus)** — `fc6730c` (fix)
2. **Task 2: resolve.c rows 22-23 + iron_net.c row 24 const-strip fix** — `abfeff5` (fix)
3. **Task 3: LIR walkthrough emit_c.c + emit_structs.c + lir_optimize.c (rows 25-34 + 2 bonus)** — `f46c86d` (fix)

## Files Created/Modified

- `src/analyzer/typecheck.c` — 13 IRON_NODE_ASSERT_KIND call sites added (rows 10-21 + 1 bonus); coverage of the type-checker generic constraint, generic instantiation, IS-narrowing, spawn-handle, and interface-completeness paths
- `src/analyzer/resolve.c` — 2 IRON_NODE_ASSERT_KIND call sites added (rows 22, 23); coverage of the enum-pattern and enum-construct variant validation paths
- `src/stdlib/iron_net.c` — Iron_tcpsocket_read rewritten: const-stripping cast removed, local heap buffer + free pattern documents the broken-by-design wrapper while eliminating silent SSO/heap corruption
- `src/lir/emit_c.c` — 4 loop-bound runtime assert sites added (rows 25, 26, 34 + 1 bonus); `assert.h` was already included
- `src/lir/emit_structs.c` — 4 loop-bound runtime assert sites + 3 IRON_NODE_ASSERT_KIND on Iron_TypeAnnotation + 2 file-scope `_Static_assert`s + new `#include <assert.h>` (rows 27, 28, 29, 29b, 30 + 1 bonus)
- `src/lir/lir_optimize.c` — 1 `_Static_assert(sizeof(load) <= sizeof(phi))` + comment block on the phi-to-load in-place rewrite (rows 31 + 32, single site) + 1 loop-bound assert at row 33

## Decisions Made

- Iron_TypeAnnotation IS an Iron_Node derivative (first field is `Iron_NodeKind kind`, IRON_NODE_TYPE_ANNOTATION enum value already exists in src/parser/ast.h). It gets IRON_NODE_ASSERT_KIND, not a runtime non-NULL assert. Iron_EnumVariant and Iron_Field do NOT have kind fields and are pure sub-structures, so they get loop-bound + non-NULL asserts only.
- The phi-to-load rewrite at lir_optimize.c:187-188 is a single rewrite site covering both audit rows 31 and 32. Consolidated into one `_Static_assert(sizeof(load) <= sizeof(phi))` + one explanatory comment block, rather than two separate asserts. The single assert is structurally the only thing that matters: it pins the union-fit invariant explicitly so future refactors that grow `load` beyond `phi` fail the build.
- iron_net.c:594 fix caps the local recv buffer at 64 KiB to avoid unbounded heap allocation if a caller passes a maliciously large Iron_String capacity. The function is broken-by-design (Iron_String is value-typed and immutable; the recv'd bytes are effectively discarded as far as the caller is concerned) and the comment block makes that explicit.
- Bonus unenumerated rows in typecheck.c (line 2359), emit_c.c (line 4525), and emit_structs.c (line 606) are covered as siblings of nearby enumerated rows — not retroactively backfilled across the entire codebase. The audit's intent is "every cast of this shape in the flagged file"; mechanical sibling coverage in the same file is in scope, broader sweeps are deferred to Phase 67.

## Deviations from Plan

None of the executed work required deviation rules — all edits followed the plan's enumerated audit rows mechanically. The 5 bonus sibling rows were explicitly anticipated by the plan's `<action>` text ("There may also be additional generic_params cast sites the audit did not enumerate ... Treat every line the grep returns as in-scope"), so they are within plan scope, not deviations.

## Issues Encountered

- The audit's pre-merge line numbers for typecheck.c rows 10-21 had drifted by ~6 lines after the WebAssembly merge (PR #23). Resolved by using `grep -n` with the pattern as the source of truth, exactly as the plan instructed.
- The audit's pre-merge line numbers for lir_optimize.c rows 31-33 had drifted further than the plan's interface block predicted (row 33 is now at line 2136, not 447). Resolved by re-grepping the actual file before editing.
- Initial chained-grep coverage census exited early on the `(uint8_t *)` zero-match line because grep returns 1 (no matches), and the chained `&&` treats that as failure. Re-ran the suffix grep separately to confirm rows 25-34 were covered. Functionally identical to running the full census; no fix needed since the absence of `(uint8_t *)(uintptr_t)base` IS the desired post-edit state.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- Cumulative PROT-03 coverage: 29 IRON_NODE_ASSERT_KIND call sites across src/ + 38 PROT-03/04 markers total. Every explicitly enumerated AUDIT-01 blind-cast row (1-34) is now structurally protected.
- Phase 67 (REG-02 crash-canary fixtures + FIX-01..04 correctness fixes) can proceed without additional structural-protection work. Any wrong-kind cast in the protected files will abort loudly via `iron_ice` in Debug builds rather than silently misreading memory.
- Plan 05 closes Phase 66. Phase 66 ROADMAP success criterion ("A Debug build that casts an AST node to the wrong kind ... hits iron_node_assert_kind and aborts") is satisfied at every audit-flagged cast site.

## Build + Test Evidence

- `cmake --build build` — clean (zero errors, only the pre-existing `ld: warning: ignoring duplicate libraries` cosmetic linker warning).
- `ctest --test-dir build --output-on-failure -j4 -E "benchmark"` — `100% tests passed, 0 tests failed out of 72`.
- `tests/integration/run_integration.sh build/ironc` — `Results: 352 passed, 0 failed, 360 total`. (8 SKIP fixtures have no `.expected` file by design.)
- `grep -rn IRON_NODE_ASSERT_KIND src/ | wc -l` — 29.
- `grep -rn "PROT-03 row\|PROT-04 rewrite\|PROT-04 walkthrough\|PROT-03 layout guard\|PROT-03 unenumerated" src/ | wc -l` — 38.

## Self-Check: PASSED

- `src/analyzer/typecheck.c` — FOUND, 13 ASSERT_KIND additions confirmed.
- `src/analyzer/resolve.c` — FOUND, 2 ASSERT_KIND additions confirmed.
- `src/stdlib/iron_net.c` — FOUND, const-strip cast removed (grep returns 0).
- `src/lir/emit_c.c` — FOUND, 4 loop-bound assert sites + PROT-03 markers confirmed.
- `src/lir/emit_structs.c` — FOUND, file-scope _Static_asserts + 4 loop-bound + 3 ASSERT_KIND confirmed.
- `src/lir/lir_optimize.c` — FOUND, _Static_assert + row 33 loop-bound assert confirmed.
- Commits `fc6730c`, `abfeff5`, `f46c86d` — all FOUND in `git log --oneline`.

---
*Phase: 66-structural-protections-linux-release-ci*
*Completed: 2026-04-13*
