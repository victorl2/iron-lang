---
phase: 12-keyword-mirrors-quickfixes
plan: 02
subsystem: lsp-completion-keyword-filter + lsp-codeaction-quickfix
tags: [keyword-filter, completion-bucket-6, quickfix, command-style-action, init-synthesis, v3-grammar, drift-guard]

# Dependency graph
requires:
  - phase: 12-keyword-mirrors-quickfixes
    plan: 01
    provides: Multi-action quickfix substrate (out_arr/out_cap/out_n, ILSP_QUICKFIX_MAX_VARIANTS=2, IronLsp_TextEdit, command_*/edit_text_edits[]/data_variant_idx fields, lifted ilsp_codeaction_derive_body_indent helper, Wave 0 stubs)
  - phase: 04-m3-editing-assistance
    provides: Bucket 6 keyword pipeline (buckets.c emit_keywords) + 5 P1 quickfix handler shape
  - phase: 11-patch-extension-support
    provides: patch_lookup.c linear-scan exemplar (patch_enclosing_in_program at lines 36-52) which keyword_filter's enclosing_object_decl mirrors verbatim
provides:
  - ilsp_keyword_visible_at(kw, doc, program, line, col, ctx) — pure NULL-safe predicate covering 6 v3 keywords + default arm for the 38 pre-v3 keywords
  - ilsp_quickfix_v3_receiver_syntax — single command-style handler shared by codes 260 (IRON_ERR_V3_RECEIVER_SYNTAX) + 261 (IRON_ERR_V3_MUT_RECEIVER) per D-18
  - ilsp_quickfix_object_no_init — single-edit synthesis handler for code 264 (IRON_ERR_V3_NO_INIT) walking Iron_ObjectDecl.fields[] filtered to is_var
  - 8-row registry.c sorted ASC by numeric code: 200, 260, 261, 264, 292, 293, 611, 612
  - 3 fixture .iron files for Plan 12-03 corpus binary
  - 15-case predicate matrix in test_v3_keyword_filter.c (Wave 0 stub flipped to real assertions)
affects: [12-03 (QF-03 + QF-04 + QF-05 + drift assertions + corpus binary flip + closeout)]

# Tech tracking
tech-stack:
  added: []  # zero new third-party deps
  patterns:
    - Linear-scan path over program->decls[] filtered by IRON_NODE_OBJECT_DECL with span containment check (mirrors patch_lookup.c:36-52 exemplar)
    - Pure byte-buffer scans for forward (`func` follows) + backward (`func (` chain) keyword visibility — bounded by doc->text_len (T-12-02-01 mitigation)
    - Command-style CodeAction shape — command_id + command_args[] = [doc->uri], no edit (D-14 / D-18); two registry rows share a single handler symbol
    - Single-edit synthesis with re-analyze-via-ilsp_facade_compile_for_nav for fresh Iron_Program; Iron_TypeAnnotation walker mirrors printer.c:185-200
    - Default-arm fall-through for NULL-doc cases (preserves Phase 4 EDIT-06 behaviour for the 38 pre-v3 keywords)

key-files:
  created:
    - src/lsp/facade/edit/complete/keyword_filter.h
    - src/lsp/facade/edit/complete/keyword_filter.c
    - src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c
    - src/lsp/facade/edit/codeaction/quickfix_object_no_init.c
    - tests/lsp/unit/v3_quickfix/qf01_receiver_syntax.iron
    - tests/lsp/unit/v3_quickfix/qf01_mut_receiver.iron
    - tests/lsp/unit/v3_quickfix/qf02_object_no_init.iron
  modified:
    - src/lsp/facade/edit/complete/buckets.c (emit_keywords gains doc/program/cursor_byte/ctx params; per-keyword filter; legacy if-gate dropped; mut detail string)
    - src/lsp/facade/edit/codeaction/registry.h (extern decls for ilsp_quickfix_v3_receiver_syntax + ilsp_quickfix_object_no_init)
    - src/lsp/facade/edit/codeaction/registry.c (3 new rows: 260, 261, 264 in correctly sorted order — note IRON_ERR_TYPE_MISMATCH_LITERAL=292, IRON_ERR_MISSING_RETURN=293 after Phase 80 renumber)
    - tests/lsp/unit/test_v3_keyword_filter.c (Wave 0 stub flipped to 15 real assertions)
    - tests/unit/test_codeaction_registry.c (8-row sort assertion + per-row handler pointer asserts)
    - CMakeLists.txt (ironls SRC list registers keyword_filter.c + 2 quickfix TUs)
    - tests/lsp/unit/CMakeLists.txt (4 add_executable blocks gain keyword_filter.c; 1 gains both new quickfix TUs; test_v3_keyword_filter sources extended for in-memory document fixtures)
    - tests/unit/CMakeLists.txt (test_codeaction_registry + test_completion_buckets gain compile.c + diagnostics.c + nav stack + keyword_filter.c + line_index.c)
    - tests/lsp/fmt/CMakeLists.txt (test_fmt_quickfix_clean gains the 2 new quickfix TUs + compile.c stack for QF-02 dependency)

key-decisions:
  - "D-04..D-09 implemented: 6 v3 keyword arms in ilsp_keyword_visible_at — pub (decl-head text-only check + enclosing OBJECT_DECL or top-level), init (enclosing OBJECT_DECL strict), readonly+pure (forward `func` scan), mut (backward `(` + `func` scan). NULL-safe."
  - "D-10 implemented: default arm preserves Phase 4 EDIT-06 behaviour bit-exactly — `(ctx == EXPR_HEAD || STATEMENT_HEAD)` for the 38 pre-v3 keywords. NULL-doc cases also fall through to the default arm to preserve test_completion_buckets cold-start behaviour."
  - "D-16..D-20 implemented: QF-01 emits exactly 1 command-style CodeAction with command_id = `iron.migrate.fromV2ToV3`, command_args = [doc->uri]. No edit. Codes 260 + 261 share the same handler (D-18 — same fix is `run the codemod`)."
  - "D-21..D-25 implemented: QF-02 walks Iron_ObjectDecl.fields[] filtered to is_var (parser.c:3993 mirror); synthesizes init body via co-located Iron_TypeAnnotation walker mirroring printer.c:185-200. Single zero-width edit at the line after the object's `{`. is_preferred=true."
  - "W-3 fix committed: enclosing_object_decl uses LINEAR-SCAN over program->decls[] (NOT a parent-walk). Iron_Node has no parent pointer (verified src/parser/ast.h:90-97). Pattern reference: src/lsp/facade/nav/patch_lookup.c:36-52."
  - "RESEARCH Open Item #3 implemented: type rendering walks Iron_TypeAnnotation (a node), not resolved Iron_Type. Co-located walker in quickfix_object_no_init.c (deferred lifting to a shared helper since it has only one consumer)."
  - "RESEARCH Open Item #4 implemented: var-only filter in QF-02 — val fields skipped (parser-side filter at parser.c:3956-3984 only counts `var` for var_field_count)."
  - "RESEARCH Open Item #10 honored: server still does NOT advertise executeCommandProvider. Editor extensions handle iron.migrate.fromV2ToV3 client-side until Phase 14."
  - "Pitfall 3 mitigated: `init` arm covers BOTH classic (is_patch=false) AND patch (is_patch=true) ObjectDecls — no is_patch filter."
  - "Pitfall 5 mitigated: mut detail string `(v2 legacy — use of `mut` emits E0263)` surfaces at buckets.c emit_keywords."
  - "Pitfall 10 mitigated: predicate tolerates broken syntax. test case `func (mu` (truncated) fires the mut arm correctly."
  - "Sort-order correction (Rule 1 fix): registry.c table comments labelled rows /* 235 */ and /* 236 */ but the actual symbol values are 292 and 293 (Phase 80 MUT renumber, see diagnostics.h:283-285). Canonical sort by numeric value is therefore 200, 260, 261, 264, 292, 293, 611, 612."
  - "D-38 preserved: zero touches to src/parser/, src/analyzer/, src/diagnostics/, src/hir/, src/lir/, src/runtime/, src/stdlib/. Parity gate (HARD-24) green throughout."

patterns-established:
  - "Single-helper module shape with optional Iron_Program param: keyword_filter.{c,h} mirrors nav/visibility.{c,h} verbatim (extern C, NULL-safe, no globals/I/O/allocation), and accepts the staged Iron_Program via the predicate signature so callers can pass whatever they already have parsed without re-analyzing."
  - "Multi-row dispatch to a single handler: registry.c rows 260 + 261 both reference ilsp_quickfix_v3_receiver_syntax; bsearch lookup still works because the table stays sorted."
  - "Synthesized-edit handler with internal re-analyze: QF-02 owns its own walk_arena + diaglist via ilsp_facade_compile_for_nav, freeing them on every refusal/success path. Caller's per-request arena is reserved exclusively for output strings."
  - "Test fixture directory convention: tests/lsp/unit/v3_quickfix/qf{NN}_*.iron with prologue comments documenting expected behavior; consumed by Plan 12-03's corpus binary flip."

requirements-completed: [KW-01, KW-02, KW-03, QF-01, QF-02]

# Metrics
duration: ~20min
completed: 2026-04-29
---

# Phase 12 Plan 12-02: Keyword Filter (KW-01..03) + QF-01 + QF-02 Summary

**Per-keyword visibility filter ships with the 6 v3 keyword arms (pub, init, readonly, pure, mut) plus a default arm that bit-exactly preserves Phase 4 EDIT-06 behaviour for the 38 pre-v3 keywords; QF-01 (command-style migrate codemod for codes 260+261) and QF-02 (var-only init synthesis for code 264) close 5 of 8 Phase 12 requirement IDs. Drift guards (KW-01, KW-02) green by default — no grammar regeneration needed.**

## Performance

- **Duration:** ~20 minutes
- **Started:** 2026-04-29T01:21:27Z (worktree base 291a7ab)
- **Completed:** 2026-04-29T01:41:34Z
- **Tasks:** 4/4 (atomic per-task commits)
- **Files changed:** 16 (7 created + 9 modified)

## Accomplishments

- **KW-01 + KW-02 verified green** without any grammar regeneration. `test_grammar_keyword_drift_textmate`, `test_grammar_keyword_drift_tree_sitter`, and `test_completion_keyword_mirror` all pass against the committed grammars + `keyword_mirror.h.in` (the 6 v3 keywords were already present in `kw_table` per RESEARCH Open Item #9).
- **KW-03 keyword_filter module** lands at `src/lsp/facade/edit/complete/keyword_filter.{c,h}` with the canonical predicate `ilsp_keyword_visible_at(kw, doc, program, line, col, ctx)`. Six v3 keyword arms cover pub/init/readonly/pure/mut; default arm covers the 38 pre-v3 keywords. Pure-read implementation: no allocation, no I/O, no globals, NULL-safe under doc=NULL (falls through to default arm).
- **buckets.c Bucket 6 integration** drops the legacy `if (ctx == EXPR_HEAD || STATEMENT_HEAD)` gate at the call site and threads doc/program/cursor_byte/ctx through to a per-keyword filter. The mut detail string `(v2 legacy — use of `mut` emits E0263)` surfaces in the completion list per Pitfall 5.
- **QF-01 quickfix_v3_receiver_syntax.c** emits exactly 1 command-style CodeAction for codes 260 + 261 (D-18: same fix is "run the codemod"). title=`Run ironc migrate --from v2 --to v3`, command_id=`iron.migrate.fromV2ToV3`, command_args=[doc->uri]. No edit (mutually exclusive with command_id per LSP 3.17 §CodeAction).
- **QF-02 quickfix_object_no_init.c** synthesizes a default `init(<f>: <T>, ...) { self.<f> = <f> ... }` block at the line after the object's `{`. Walks `Iron_ObjectDecl.fields[]` filtered to `is_var == true`; type rendering via co-located `Iron_TypeAnnotation` walker (RESEARCH Open Item #3 — operates on the AST type-annotation, not the resolved Iron_Type). Body indent derived via Plan 12-01's lifted `ilsp_codeaction_derive_body_indent`.
- **registry.c table grew from 5 rows to 8** in correctly sorted ASC order: 200, 260, 261, 264, 292, 293, 611, 612. Discovered + corrected a stale comment in registry.c that labelled the 5th and 6th rows as `/* 235 */` and `/* 236 */` even though the actual symbol values were 292 and 293 after the Phase 80 MUT renumber (Rule 1 bug fix).
- **15-case predicate matrix** in `test_v3_keyword_filter.c` exercises pub/init/readonly/pure/mut/default + NULL-safety + Pitfall 10 broken syntax (`func (mu`). Wave 0 stub flipped to real assertions; binary reports REAL pass count instead of TEST_IGNORE.
- **Parity gate (HARD-24 / D-38) preserved by construction.** `test_parity_ironc_lsp` + `test_parity_ironc_lsp_fmt` + `test_parity_ironc_lsp_suggestions` all 3 green throughout the 4 commits. Zero touches to `src/parser/`, `src/analyzer/`, `src/diagnostics/`, `src/hir/`, `src/lir/`, `src/runtime/`, `src/stdlib/`.

## Task Commits

Each task was committed atomically (--no-verify due to parallel-executor worktree):

1. **Task 1: KW-03 keyword_filter module + buckets.c per-keyword filter** — `52db398` (feat)
2. **Task 2: QF-01 quickfix_v3_receiver_syntax for codes 260+261** — `27197fd` (feat)
3. **Task 3: QF-02 quickfix_object_no_init for code 264** — `316a437` (feat)
4. **Task 4: Plan close — keyword_filter NULL-doc graceful, test wiring** — `e3c2898` (chore)

## Files Created/Modified

### Created (7)

- `src/lsp/facade/edit/complete/keyword_filter.h` — single-helper module header (mirrors nav/visibility.h shape); declares `ilsp_keyword_visible_at(kw, doc, program, line, col, ctx)` under `extern "C"`. Header guard `IRON_LSP_FACADE_EDIT_COMPLETE_KEYWORD_FILTER_H`.
- `src/lsp/facade/edit/complete/keyword_filter.c` — predicate impl. 6 v3 keyword arms + default arm. Pure read (no allocation, no I/O, no globals); NULL-safe with doc=NULL graceful fall-through.
- `src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c` — command-style handler. UTF-8-encoded `→` arrow in command_title via `\xe2\x86\x92`.
- `src/lsp/facade/edit/codeaction/quickfix_object_no_init.c` — single-edit synthesis handler. Owns walk_arena + diaglist for ilsp_facade_compile_for_nav; co-located `type_ann_to_string` + `find_object_for_diag` static helpers.
- `tests/lsp/unit/v3_quickfix/qf01_receiver_syntax.iron` — fixture for E0260.
- `tests/lsp/unit/v3_quickfix/qf01_mut_receiver.iron` — fixture for E0261.
- `tests/lsp/unit/v3_quickfix/qf02_object_no_init.iron` — fixture for E0264 (var-only fields, no init).

### Modified (9)

- `src/lsp/facade/edit/complete/buckets.c` — `emit_keywords` signature widened to `(out_arr, arena, doc, program, cursor_byte, ctx, query_prefix, cancel)`. Converts cursor_byte → (line, col) once via ilsp_line_of_byte / ilsp_byte_of_line. Per-keyword filter via `ilsp_keyword_visible_at`. Legacy if-gate dropped at the call site. Includes `keyword_filter.h` + `lsp/store/line_index.h`.
- `src/lsp/facade/edit/codeaction/registry.h` — 2 new extern decls (`ilsp_quickfix_v3_receiver_syntax`, `ilsp_quickfix_object_no_init`).
- `src/lsp/facade/edit/codeaction/registry.c` — 3 new rows in sorted-by-numeric-value order (200, 260, 261, 264, 292, 293, 611, 612). Stale `/* 235 */` and `/* 236 */` comments corrected to `/* 292 */` and `/* 293 */` to match the symbols' actual values after Phase 80 MUT renumber.
- `tests/lsp/unit/test_v3_keyword_filter.c` — Wave 0 stub flipped to 15-case predicate matrix.
- `tests/unit/test_codeaction_registry.c` — `test_table_sorted_asc_by_code` updated: expects 8 rows in canonical order; per-row handler-pointer assertions for codes 260, 261 (shared handler), 264.
- `CMakeLists.txt` — ironls SRC list registers keyword_filter.c + 2 new quickfix TUs.
- `tests/lsp/unit/CMakeLists.txt` — _LSP_PLAN03_SERVER_SRC adds keyword_filter.c + both new quickfix TUs (consumed by 6+ test binaries). test_v3_keyword_filter sources extended with keyword_filter.c, document.c, sha256.c. test_v3_patch_completion + test_v3_tier_completion gain keyword_filter.c.
- `tests/unit/CMakeLists.txt` — test_codeaction_registry gains compile.c, diagnostics.c, nav/{nav_common,node_at}.c, transport+obs stack (for QF-02 ilsp_facade_compile_for_nav dependency); also gains the 2 new quickfix TUs. test_completion_buckets gains keyword_filter.c + line_index.c.
- `tests/lsp/fmt/CMakeLists.txt` — test_fmt_quickfix_clean gains the 2 new quickfix TUs + compile.c stack for QF-02 dependency.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Plan signature `(kw, doc, line, col, ctx)` lacked Iron_Program access**
- **Found during:** Task 1 (writing keyword_filter.h)
- **Issue:** Plan specified `ilsp_keyword_visible_at(kw, doc, line, col, ctx)` and recommended consulting `ilsp_document_staged_program(doc)`, but `IronLsp_Document` does not store a staged Iron_Program (verified via `grep ilsp_document_staged_program` — no such accessor exists).
- **Fix:** Widened the predicate signature to `ilsp_keyword_visible_at(kw, doc, program, line, col, ctx)`. The caller (`emit_keywords` in buckets.c) already has `program` in its outer scope; threading it through is a 1-line change. NULL `program` triggers lenient/strict fallback per arm (per the original plan's intent).
- **Files modified:** `src/lsp/facade/edit/complete/keyword_filter.h`, `src/lsp/facade/edit/complete/keyword_filter.c`, `src/lsp/facade/edit/complete/buckets.c`
- **Commit:** `52db398`

**2. [Rule 3 - Blocking] Plan said cursor_line/cursor_col already in scope, but buckets entry has only cursor_byte_offset**
- **Found during:** Task 1 (modifying buckets.c emit_keywords)
- **Issue:** `ilsp_complete_buckets_build` takes `cursor_byte_offset` (size_t), not (line, col). The plan's claim that "doc, cursor_line, cursor_col, and ctx variables are already available in the surrounding function scope" was incorrect.
- **Fix:** Inside emit_keywords, derive cursor_line/cursor_col once from cursor_byte via ilsp_line_of_byte / ilsp_byte_of_line. The conversion is bounded + NULL-safe.
- **Files modified:** `src/lsp/facade/edit/complete/buckets.c`
- **Commit:** `52db398`

**3. [Rule 1 - Bug] Stale row labels in registry.c and incorrect insertion order**
- **Found during:** Task 2 (verifying test_codeaction_registry)
- **Issue:** registry.c originally had `/* 235 */ { IRON_ERR_TYPE_MISMATCH_LITERAL, ... }` and `/* 236 */ { IRON_ERR_MISSING_RETURN, ... }`, but the symbol values are 292 and 293 after the Phase 80 MUT renumber (see diagnostics.h:283-285). The plan recommended inserting 260/261 between row 236 (= 293) and row 611, which would have produced `200, 292, 293, 260, 261, 611, 612` — not sorted by numeric value, so bsearch would silently fail to find codes 292 and 293.
- **Fix:** Sorted rows by actual numeric value: `200, 260, 261, 292, 293, 611, 612`. Updated stale comment labels to `/* 292 */` and `/* 293 */`. Added a defensive comment near the table noting the renumber. Updated `test_table_sorted_asc_by_code` to expect 7 rows (then 8 after Task 3).
- **Files modified:** `src/lsp/facade/edit/codeaction/registry.c`, `tests/unit/test_codeaction_registry.c`
- **Commit:** `27197fd`

**4. [Rule 3 - Blocking] keyword_filter NULL-doc returned false for ALL keywords, breaking test_completion_buckets**
- **Found during:** Task 4 (full m1-invariant sweep)
- **Issue:** `test_expr_head_emits_keywords` and `test_cold_start_safety` in `tests/unit/test_completion_buckets.c` call `ilsp_complete_buckets_build` with `doc = NULL`. My initial predicate signature returned false for ALL keywords when `doc == NULL`, so the keyword bucket emitted zero candidates — these tests asserted at least one keyword fires under EXPR_HEAD.
- **Fix:** When `doc == NULL`, refuse the 5 v3 arms that need a byte buffer or staged program (pub/init/readonly/pure/mut) by name and fall through to the default arm. The `patch` keyword (sixth v3 keyword) is context-free at decl-head and naturally falls through to the default arm.
- **Files modified:** `src/lsp/facade/edit/complete/keyword_filter.c`
- **Commit:** `e3c2898`

**5. [Rule 3 - Blocking] Multiple test binaries link buckets.c without keyword_filter.c**
- **Found during:** Task 1 + Task 4 build sweep
- **Issue:** After Task 1 added the `ilsp_keyword_visible_at` call to buckets.c, every test binary that links `buckets.c` failed to link with `undefined reference to ilsp_keyword_visible_at`. Affected binaries: test_v3_quickfix_corpus (via _LSP_PLAN03_SERVER_SRC), test_v3_patch_completion, test_v3_tier_completion (via direct buckets.c reference), test_completion_buckets (tests/unit).
- **Fix:** Added `keyword_filter.c` to all 4 source list locations. test_completion_buckets additionally needed `lsp/store/line_index.c` for `ilsp_byte_of_line` / `ilsp_line_of_byte`.
- **Files modified:** `tests/lsp/unit/CMakeLists.txt`, `tests/unit/CMakeLists.txt`
- **Commits:** `52db398` (Task 1), `e3c2898` (Task 4)

**6. [Rule 3 - Blocking] test_codeaction_registry + test_fmt_quickfix_clean link registry.c which references QF-02's compile-stack symbols**
- **Found during:** Task 3 (build sweep after adding QF-02)
- **Issue:** `quickfix_object_no_init.c` calls `ilsp_facade_compile_for_nav` (re-analyze the document for a fresh Iron_Program). Registry.c references `ilsp_quickfix_object_no_init` (now in the table), so test_codeaction_registry and test_fmt_quickfix_clean failed to link with `undefined reference to ilsp_facade_compile_for_nav`.
- **Fix:** Added compile.c + diagnostics.c + nav/nav_common.c + nav/node_at.c + transport (json.c, writer.c, frame.c) + obs (log.c) + yyjson.c to both test binaries. Mirrors `test_rename_prepare`'s link shape (tests/unit/CMakeLists.txt:532-551). Marked yyjson.c with `-Wno-error -w` per existing convention.
- **Files modified:** `tests/unit/CMakeLists.txt`, `tests/lsp/fmt/CMakeLists.txt`
- **Commit:** `316a437`

**7. [Rule 3 - Blocking] test_mut_broken_syntax_partial_ident_true fixture mismatched the predicate spec**
- **Found during:** Task 1 (running test_v3_keyword_filter)
- **Issue:** Initial test fixture was `func update(mu` — but the v2 receiver syntax requires `mut` to appear IMMEDIATELY inside the receiver parens (no method name between `func` and `(`). The text `func update(mu` is a regular function with `mu` as a partial param name, which the predicate correctly refuses.
- **Fix:** Updated fixture to `func (mu` — truncated source mid-typing the `mut` receiver keyword. This matches the actual v2 receiver-syntax pattern that `mut` would auto-complete in.
- **Files modified:** `tests/lsp/unit/test_v3_keyword_filter.c`
- **Commit:** `52db398`

### Out-of-scope discoveries (logged for tracking, not fixed)

See `.planning/phases/12-keyword-mirrors-quickfixes/deferred-items.md`:
- **test_string_intern_race** — TSAN runtime not installed (Plan 12-01 noted)
- **test_ast_sealed** — pre-existing `-Werror=address` failure on stack-var address in macro
- **test_v3_symbol_id_corpus** — pre-existing failure at worktree base 291a7ab
- **test_phase7_audit** — pre-existing failure at worktree base 291a7ab

All 4 reproducible without Plan 12-02 changes (verified via `git stash`).

## Verification

### Build (D-38 / parity-gate-safe)

```
cmake --build build --target ironls
```

- **Exit code:** 0
- **Warnings:** 0 (under `-Wall -Wextra -Werror -Wpedantic -Werror=switch-enum`)

### CTest sweep (Plan 12-02 success criteria)

```
ctest --test-dir build -L phase-m1-invariant       # 27/27 passed
ctest --test-dir build -L phase-12-invariant       # 3/3 passed
ctest --test-dir build -R test_parity_ironc_lsp    # 3/3 passed (HARD-24 / D-38)
ctest --test-dir build -R test_grammar_keyword_drift # 2/2 passed (KW-01 closed)
ctest --test-dir build -R test_completion_keyword_mirror # 1/1 passed (KW-02 closed)
ctest --test-dir build -R test_v3_keyword_filter   # 1/1 passed (KW-03 closed; 15 real assertions)
ctest --test-dir build -R test_codeaction_registry # 1/1 passed (8-row table; QF-01 + QF-02 registered)
ctest --test-dir build -R test_v3_quickfix_corpus  # 1/1 passed (links cleanly; Plan 12-03 wires assertions)
ctest --test-dir build -R test_codeaction          # 5 existing handlers regression: 1/1 passed
ctest --test-dir build -R test_organize_imports    # 1/1 passed
ctest --test-dir build -R test_fmt_quickfix_clean  # 1/1 passed
ctest --test-dir build -R test_completion_buckets  # 1/1 passed (cold-start fallback preserved)
```

### Patch-shape grep gates

```
$ grep -q "ilsp_keyword_visible_at" src/lsp/facade/edit/complete/buckets.c && echo OK
OK
$ grep -q "ilsp_keyword_visible_at" src/lsp/facade/edit/complete/keyword_filter.h && echo OK
OK
$ grep -q "v2 legacy" src/lsp/facade/edit/complete/buckets.c && echo OK
OK
$ grep -q "program->decls" src/lsp/facade/edit/complete/keyword_filter.c && echo OK
OK   # linear-scan path committed
$ ! grep -qE "->parent|ilsp_nav_node_at" src/lsp/facade/edit/complete/keyword_filter.c && echo OK
OK   # parent-walk path NOT used (W-3 fix verified)
$ grep -q "iron.migrate.fromV2ToV3" src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c && echo OK
OK
$ grep -c "ilsp_quickfix_v3_receiver_syntax" src/lsp/facade/edit/codeaction/registry.c
2   # codes 260 + 261 share the handler (D-18)
$ grep -q "IRON_ERR_V3_NO_INIT" src/lsp/facade/edit/codeaction/registry.c && echo OK
OK
$ grep -q "is_var" src/lsp/facade/edit/codeaction/quickfix_object_no_init.c && echo OK
OK   # var-only filter committed
$ grep -q "type_ann_to_string" src/lsp/facade/edit/codeaction/quickfix_object_no_init.c && echo OK
OK   # Iron_TypeAnnotation walker committed
```

## Patterns Followed

- **PATTERNS.md "Single-helper module shape (predicate/utility module)"** — `keyword_filter.{c,h}` mirrors `nav/visibility.{c,h}` verbatim: phase provenance comment + extern "C" + forward-declared consumed struct + NULL-safe pure-read body + safe under concurrent request threads.
- **PATTERNS.md "Quickfix handler TU shape"** — both QF-01 and QF-02 follow the sentinel-preamble + `out_arr[0]` field-rewrite + `*out_n = 1` terminator pattern. QF-01 is command-style (only command_*); QF-02 is legacy single-edit (only edit_*).
- **PATTERNS.md "buckets.c (Bucket 6 integration)"** — verbatim shape: emit_keywords gains 4 new params; per-keyword filter call replaces the per-keyword maybe_push gate; mut detail string surfaces.
- **PATTERNS.md "registry.c (sorted-asc dispatch table)"** — 3 new rows inserted in numerically sorted order; bsearch lookup invariant preserved.
- **PATTERNS.md "patch_lookup.c:36-52 linear-scan exemplar"** — `enclosing_object_decl` mirrors the iteration shape verbatim (loop over program->decls[], filter by IRON_NODE_OBJECT_DECL, span containment check).
- **RESEARCH.md Open Items #3, #4, #9, #10 honored:**
    - #3: type rendering walks Iron_TypeAnnotation, not resolved Iron_Type.
    - #4: var-only filter in QF-02.
    - #9: drift guards green by default; no grammar regen needed.
    - #10: server still does NOT advertise executeCommandProvider.
- **RESEARCH.md Pitfalls 3, 5, 9, 10 mitigated:**
    - Pitfall 3 (init in patches): no is_patch filter in init arm.
    - Pitfall 5 (mut detail): `(v2 legacy — use of `mut` emits E0263)` surfaces.
    - Pitfall 9 (var-only): QF-02 filters f->is_var == true.
    - Pitfall 10 (broken syntax): test fixture `func (mu` exercises predicate tolerance.

## Confirmed: Parity-Gate Safety (D-38)

`git diff 291a7ab..HEAD -- src/parser/ src/analyzer/ src/diagnostics/ src/hir/ src/lir/ src/runtime/ src/stdlib/` shows zero changes. The compiler frontend / middle-end / backend / runtime / stdlib are untouched. `test_parity_ironc_lsp` + `test_parity_ironc_lsp_fmt` + `test_parity_ironc_lsp_suggestions` green throughout the 4 commits.

## Self-Check: PASSED

All claimed file paths exist:
- `src/lsp/facade/edit/complete/keyword_filter.h` — FOUND
- `src/lsp/facade/edit/complete/keyword_filter.c` — FOUND
- `src/lsp/facade/edit/codeaction/quickfix_v3_receiver_syntax.c` — FOUND
- `src/lsp/facade/edit/codeaction/quickfix_object_no_init.c` — FOUND
- `tests/lsp/unit/v3_quickfix/qf01_receiver_syntax.iron` — FOUND
- `tests/lsp/unit/v3_quickfix/qf01_mut_receiver.iron` — FOUND
- `tests/lsp/unit/v3_quickfix/qf02_object_no_init.iron` — FOUND

All claimed commit hashes exist:
- `52db398` (Task 1) — FOUND
- `27197fd` (Task 2) — FOUND
- `316a437` (Task 3) — FOUND
- `e3c2898` (Task 4) — FOUND

5 of 8 phase requirements closed: KW-01, KW-02, KW-03, QF-01, QF-02. Plan 12-03 ships QF-03, QF-04, QF-05 + drift assertions + corpus binary flip + closeout.
