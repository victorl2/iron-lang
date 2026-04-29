# Phase 12 — Deferred Items

Out-of-scope discoveries logged during plan execution + items deferred per
CONTEXT.md `<deferred>` section. Future phases pick these up by name.

## Pre-existing failures (Plan 12-02 + 12-03 sweep, 2026-04-29)

These existed at the worktree base BEFORE any Phase 12 work and are
unrelated to KW-01..03 + QF-01..05. Verified by re-running ctest at the
Plan 12-02 closeout commit (e09c2c4) — same 14 failures.

- **test_string_intern_race** — fails to link with `cannot find
  /usr/lib64/libtsan.so.0.0.0`. Pre-existing environment issue (TSAN
  runtime not installed on this dev system); already noted in Plan
  12-01 SUMMARY. Not a Phase 12 regression.

- **test_ast_sealed** — pre-existing compile error at
  `src/parser/ast.h:176` under `-Werror=address`: `the address of 'p'
  will always evaluate as 'true'`. The test harness passes a stack
  variable address into the IRON_AST_ASSERT_UNSEALED macro which
  triggers `-Werror=address`. Owner: parser team.

- **test_v3_symbol_id_corpus** — pre-existing failure at the worktree
  base. Owner: nav/symbol_id team.

- **test_phase7_audit** — pre-existing failure at the worktree base.
  Owner: phase 7 audit framework.

- **test_iron_build_package**, **test_iron_run_package**,
  **test_iron_build_no_deps_still_works**, **test_iron_dep_lockfile**,
  **test_cli_parse_web** — package-manager / web-target integration
  tests; pre-existing at base. Owner: cli + pkg teams.

- **benchmark_smoke** — pre-existing benchmark harness issue at base.

- **test_integration**, **test_algorithms**, **test_composite**,
  **test_manual** — pre-existing integration corpus failures.

These items are tracked here for visibility; they do NOT block Phase 12
gate criteria (parity gate green, m1-invariant 100% pass, all 8 Phase
12 requirement IDs closed).

## Deferred to Phase 14 (Migration + Codemod + v3.0.0-alpha.1 Release)

| ID | Item | Owner Phase | Notes |
|----|------|-------------|-------|
| DEF-12-01 | Server-side `workspace/executeCommand` dispatch handler | Phase 14 CMD-01 | QF-01 emits command_id; server does not advertise executeCommandProvider yet |
| DEF-12-02 | `executeCommandProvider` capability advertisement | Phase 14 CMD-01 | Pairs with DEF-12-01 |
| DEF-12-03 | `$/progress` streaming during codemod execution | Phase 14 CMD-02 | Editor extensions handle progress UX client-side until Phase 14 |
| DEF-12-04 | `WorkspaceEdit` preview-before-apply for migrate | Phase 14 CMD-03 | Same |

## Deferred to Backlog

| ID | Item | Notes |
|----|------|-------|
| DEF-12-05 | Editor extension command palette wiring for `iron.migrate.fromV2ToV3` | Could ship alongside Phase 14 to give the migration UX a one-click experience without waiting for server-side dispatch; backlog candidate |
| DEF-12-06 | Single-keystroke fix for `IRON_ERR_V3_MUT_KEYWORD = 263` | Same migration path as QF-01 covers it; backlog |
| DEF-12-07 | Single-keystroke fix for `IRON_ERR_PATCH_TARGET_NOT_FOUND = 254` | Less common error; backlog |
| DEF-12-08 | Single-keystroke fix for `IRON_ERR_TIER_MODIFIER_PLACEMENT = 245` | v3 grammar disallows; rare error; backlog |
| DEF-12-09 | Keyword-filter perf cache | Predicate is O(line-tokens) per call; profile before optimizing; backlog |
| DEF-12-10 | Lift `derive_body_indent` already done in Plan 12-01 | CLOSED |
| DEF-12-11 | Cross-file QF-04/QF-05 Action B requires per-edit URI on IronLsp_CodeAction | Phase 12 v1 limited Action B to same-file callees; full cross-file requires extending `IronLsp_CodeAction` with an optional `target_uri` field so the wire serializer can emit a per-edit `documentChanges[].textDocument.uri`. Cross-file fixtures (qf05_readonly_calls_mutating_cross_file/) currently exercise the "1 action only" path. |
| DEF-12-12 | Quickfix for "this keyword is in the wrong context" warnings (e.g., `init` typed at module top-level) | Backlog (low-value error) |
| DEF-12-13 | Per-keyword detail strings for the 38 pre-v3 keywords | Phase 12 only added `mut` detail (legacy marker); other 37 stay un-detailed; backlog if editor UX requests |
| DEF-12-14 | Refactoring `data_diagnostic_idx` round-trip to string-encoded payload | Current shape (4-int triple + variant_idx) works for Phase 12; backlog |
| DEF-12-15 | Lift `type_ann_to_string` to a shared TU | Phase 12 has 2 consumers (quickfix_object_no_init.c + quickfix_v3_inline_default.c); lift threshold = next phase adding a 3rd consumer |
| DEF-12-16 | Lift `find_enclosing_method` + `find_readonly_token` + `same_file` helpers to a shared TU | QF-04 + QF-05 currently duplicate these helpers verbatim. Lift threshold = next phase adding a 3rd consumer (likely a future tier-modifier quickfix). |

---
*Phase: 12-keyword-mirrors-quickfixes*
*Closed: 2026-04-29 (Plan 12-03)*
