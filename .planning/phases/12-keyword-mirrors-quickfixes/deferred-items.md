# Phase 12 — Deferred Items

Out-of-scope discoveries logged during plan execution. Do NOT fix in
Phase 12; these are pre-existing failures unrelated to KW-01..03 +
QF-01..05.

## Pre-existing failures (Plan 12-02 sweep, 2026-04-29)

- **test_string_intern_race** — fails to link with `cannot find
  /usr/lib64/libtsan.so.0.0.0`. Pre-existing environment issue (TSAN
  runtime not installed on this dev system); already noted in Plan
  12-01 SUMMARY. Not a Phase 12 regression.

- **test_ast_sealed** — pre-existing compile error at
  `src/parser/ast.h:176` under `-Werror=address`: `the address of 'p'
  will always evaluate as 'true'`. The test harness passes a stack
  variable address into the IRON_AST_ASSERT_UNSEALED macro which
  triggers `-Werror=address`. Reproducible without any Phase 12
  changes (verified via `git stash` against the worktree base
  291a7ab). Owner: parser team.

- **test_v3_symbol_id_corpus** — pre-existing failure at the worktree
  base. Reproducible without Phase 12 changes. Owner: nav/symbol_id
  team.

- **test_phase7_audit** — pre-existing failure at the worktree base.
  Reproducible without Phase 12 changes. Owner: phase 7 audit
  framework.

These items are tracked here for visibility; they do NOT block Plan
12-02 gate criteria (parity gate green, m1-invariant 100% pass,
KW-01..03 + QF-01 + QF-02 closed).
