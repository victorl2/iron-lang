---
plan: 12-03-multi-action-quickfixes-and-closeout
phase: 12-keyword-mirrors-quickfixes
status: complete
wave: 3
created: 2026-04-29
requirements: [QF-03, QF-04, QF-05]
---

# Plan 12-03 — Summary

> Wave 3 of Phase 12 — multi-edit + multi-action quickfix handlers + Phase 12 closeout.

## What was built

Three new quickfix handlers (QF-03, QF-04, QF-05) consuming the Plan 12-01 widened ABI (`out_arr` / `out_cap` / `*out_n`, `edit_text_edits[]`, `data_variant_idx`):

- **QF-03 — `quickfix_v3_inline_default.c` (code 262)** — multi-edit handler for `IRON_ERR_V3_INLINE_DEFAULT`. Removes the inline `= <expr>` from a field declaration AND inserts/extends an `init(...)` body so `self.<field> = <expr>` lands in the right place. Reuses `codeaction_indent.h` for body indent. Two TextEdits per CodeAction emitted via the wire's `documentChanges[]` shape.

- **QF-04 — `quickfix_readonly_write_self.c` (code 238)** — two-action handler for `IRON_ERR_READONLY_WRITE_SELF`. Action A removes the `readonly` token from the enclosing method signature; Action B removes the offending write statement. Both `is_preferred = false` (semantic ambiguity — user picks).

- **QF-05 — `quickfix_readonly_calls_mutating.c` (code 239)** — two-action handler for `IRON_ERR_READONLY_CALLS_MUTATING`. Action A drops `readonly` from the caller; Action B inserts `readonly ` before the callee's `func` token. Action B emitted ONLY when the callee is in the same file (per the W-2 fix from plan-checker — `md->span.filename == diag->span.filename` pointer-equality, since `Iron_Span.filename` is arena-interned). Cross-file callees → Action A only. Stdlib callees → Action A only (D-34 carve-out via `ilsp_nav_path_is_stdlib`). Callee resolution walks `program->decls[]` filtered to `IRON_NODE_METHOD_DECL` matching `method_name` (declaration order, first wins) — methods are HOISTED to top-level decls per Phase 9 D-06/D-07; Iron_ObjectDecl has NO `methods[]` field.

Registry table grew from 8 rows (Plan 12-02) to 11 rows in final ASC order:
`200, 238, 239, 260, 261, 262, 264, 292, 293, 611, 612` (codes 235→292 and 236→293 were renumbered per F3 Phase 8 rebase; sort discipline preserved).

Wave 0 stub binaries flipped to real assertions:
- `test_v3_quickfix_corpus.c` — 9 fixtures × per-fixture assertions (qf01_receiver_syntax, qf01_mut_receiver, qf02_object_no_init, qf03_inline_default_no_init, qf03_inline_default_with_init, qf04_readonly_write_self, qf05_readonly_calls_mutating_local, qf05_readonly_calls_mutating_cross_file/{mod_a,mod_b}, qf05_readonly_calls_mutating_stdlib).
- `test_v3_keyword_drift.c` — fast in-process drift assertion confirming all 6 v3 keywords present in `keyword_mirror.h` + grammars.

## Commits

| SHA | Subject |
|-----|---------|
| `5e4d853` | feat(12-03): QF-03 quickfix_v3_inline_default for code 262 (Task 1) |
| `38251fa` | feat(12-03): QF-04 readonly_write_self + QF-05 readonly_calls_mutating (Task 2) |
| `5bc41e1` | test(12-03): flip wave 0 stubs to real corpus + drift assertions (Task 3) |
| (this) | docs(12-03): close Phase 12 — deferred-items + SUMMARY (Task 4) |

## Verification

- `cmake --build build --target ironls` — 0 warnings, 0 errors
- `ctest -L phase-12-invariant` — 3/3 (test_v3_quickfix_corpus, test_v3_keyword_filter, test_v3_keyword_drift)
- `ctest -R test_parity_ironc_lsp` — 3/3 (HARD-24 / D-38 preserved)
- Registry sort: 11 rows in correct ASC order (verified via grep on `registry.c`)
- All 9 fixtures present under `tests/lsp/unit/v3_quickfix/` (incl. cross-file dir)
- All 5 existing pre-Phase-12 quickfixes still emit 1 action each (regression guard for D-12 signature shift)

## Phase 12 — All 8 requirement IDs closed

| ID | Status | Evidence |
|----|--------|----------|
| KW-01 | ✓ | `test_grammar_keyword_drift_textmate` + `_tree_sitter` green; 6 v3 keywords flow through configure_file extractor |
| KW-02 | ✓ | `test_completion_keyword_mirror` byte-parity green; `keyword_mirror.h` regenerates with all 44 keywords |
| KW-03 | ✓ | `test_v3_keyword_filter` predicate matrix 15/15; `mut` legacy detail string emitted |
| QF-01 | ✓ | E0260 + E0261 → 1 CodeAction with `command_id = "iron.migrate.fromV2ToV3"` |
| QF-02 | ✓ | E0264 → 1 CodeAction; synthesized init body matches `Iron_ObjectDecl.fields[]` filtered to `f->is_var == true` |
| QF-03 | ✓ | E0262 → 1 CodeAction with multi-edit (remove inline + extend init) |
| QF-04 | ✓ | E0238 → 2 CodeActions (drop modifier + remove write) |
| QF-05 | ✓ | E0239 → 2 actions same-file; 1 action cross-file (DEF-12-11 deferral); 1 action stdlib (D-34 carve-out) |

## Deviations from Plan

None of substance. All 4 plan tasks executed as specified. Task 4 closeout was completed inline by the orchestrator after the executor agent hit a session rate limit between Tasks 3 and 4 — Tasks 1, 2, 3 had committed cleanly via the worktree, and Task 4's deferred-items.md edits were preserved in the merge. `.planning/REQUIREMENTS.md` was already in the correct state at the worktree base (pre-populated when Phase 12 was added to the roadmap), so no flip commit was necessary.

## Cross-cutting contracts preserved

- **D-38 parity gate sacred** — zero diff in `src/{parser,analyzer,diagnostics,hir,lir,runtime,stdlib}/`. Phase 12 ships entirely inside `src/lsp/` + `tests/lsp/unit/v3_quickfix/`.
- **D-39 extension version-range constants UNCHANGED** — no edits to `editors/{vscode,neovim,zed}/*`.
- **D-41 no new XXX_PHASE_12 markers seeded.**
- **HARD-04 OOM discipline** — all new handlers allocate via per-request arena.
- **HARD-05 cancel-flag threading** — preserved in QF-05 cross-file callee walk (bounded by `program->decl_count`).
- **Sealed-tree invariant** — every new handler reads `program->sealed` immutably; no AST mutation.
- **Switch-enum strictness** — no new switches over `Iron_NodeKind` introduced.

## Files

### Created
- `src/lsp/facade/edit/codeaction/quickfix_v3_inline_default.c`
- `src/lsp/facade/edit/codeaction/quickfix_readonly_write_self.c`
- `src/lsp/facade/edit/codeaction/quickfix_readonly_calls_mutating.c`
- `tests/lsp/unit/v3_quickfix/qf03_inline_default_no_init.iron`
- `tests/lsp/unit/v3_quickfix/qf03_inline_default_with_init.iron`
- `tests/lsp/unit/v3_quickfix/qf04_readonly_write_self.iron`
- `tests/lsp/unit/v3_quickfix/qf05_readonly_calls_mutating_local.iron`
- `tests/lsp/unit/v3_quickfix/qf05_readonly_calls_mutating_cross_file/mod_a.iron`
- `tests/lsp/unit/v3_quickfix/qf05_readonly_calls_mutating_cross_file/mod_b.iron`
- `tests/lsp/unit/v3_quickfix/qf05_readonly_calls_mutating_stdlib.iron`
- `.planning/phases/12-keyword-mirrors-quickfixes/12-03-SUMMARY.md`

### Modified
- `src/lsp/facade/edit/codeaction/registry.h` — added 3 handler declarations (signature unchanged)
- `src/lsp/facade/edit/codeaction/registry.c` — inserted rows 238, 239, 262 in ASC sort order
- `tests/lsp/unit/test_v3_quickfix_corpus.c` — flipped from Wave 0 stub to per-fixture assertions
- `tests/lsp/unit/test_v3_keyword_drift.c` — flipped from Wave 0 stub to real drift cross-check
- `tests/lsp/unit/CMakeLists.txt` — added new fixture paths to test data registration
- `.planning/phases/12-keyword-mirrors-quickfixes/deferred-items.md` — added DEF-12-10..DEF-12-16 (6 new deferrals incl. cross-file QF-05 DEF-12-11)

## Next

Phase 12 closeout complete. Ready for `/gsd-secure-phase 12` (security review) and phase verification gate.
