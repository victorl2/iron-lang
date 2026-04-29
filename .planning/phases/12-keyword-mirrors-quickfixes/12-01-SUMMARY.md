---
phase: 12-keyword-mirrors-quickfixes
plan: 01
subsystem: lsp-codeaction-substrate
tags: [codeaction, quickfix, multi-action, lsp-3.17, command-action, multi-edit, variant-disambiguation, infrastructure-widening]

# Dependency graph
requires:
  - phase: 04-m3-editing-assistance
    provides: Bucket 6 keyword pipeline + 5 P1 quickfix handlers + codeAction/resolve lazy-edit framework (D-06, D-07)
  - phase: 11-patch-extension-support
    provides: Wave 0 test-binary template (test_v3_patch_predicate.c) + dual-label CTest invariant pattern
provides:
  - Multi-action quickfix substrate (IronLsp_QuickfixFn (out_arr, out_cap, out_n) signature; ILSP_QUICKFIX_MAX_VARIANTS = 2)
  - IronLsp_CodeAction extensions: command_* fields, edit_text_edits[] array, data_variant_idx (all backwards compatible additive)
  - IronLsp_TextEdit struct (multi-edit element)
  - Wire serializer support for `command:{}` and N-entry `edits[]` envelopes; `variant_idx` round-trip key
  - Lifted body-indent helper at src/lsp/facade/edit/codeaction/codeaction_indent.{c,h}
  - 3 Wave 0 test-binary stubs (test_v3_quickfix_corpus, test_v3_keyword_filter, test_v3_keyword_drift) wired under phase-m1-invariant + phase-12-invariant labels
affects: [12-02 (KW-01..03 + QF-01 + QF-02), 12-03 (QF-03 + QF-04 + QF-05 + closeout)]

# Tech tracking
tech-stack:
  added: []  # zero new third-party deps
  patterns:
    - Multi-action quickfix emit (out_arr, out_cap, out_n) replacing single-out
    - Mutually-exclusive command-style / multi-edit / legacy single-edit branches in IronLsp_CodeAction
    - Per-action variant_idx lazy-resolve round-trip (data_diagnostic_idx + data_variant_idx quad)
    - Wave 0 stub binary with TEST_IGNORE_MESSAGE under dual phase-m1 + phase-12 invariant labels (verbatim Phase 11 template)
    - Single-helper module shape for codeaction_indent.{c,h} mirroring nav/visibility.{c,h}

key-files:
  created:
    - src/lsp/facade/edit/codeaction/codeaction_indent.h
    - src/lsp/facade/edit/codeaction/codeaction_indent.c
    - tests/lsp/unit/test_v3_quickfix_corpus.c
    - tests/lsp/unit/test_v3_keyword_filter.c
    - tests/lsp/unit/test_v3_keyword_drift.c
  modified:
    - src/lsp/facade/edit/codeaction/registry.h (typedef widened, struct grew with 3 optional branches + variant_idx)
    - src/lsp/facade/edit/codeaction/registry.c (no functional change; const table still 5 rows)
    - src/lsp/facade/edit/codeaction/codeaction.h (resolve facade gains data_variant_idx parameter)
    - src/lsp/facade/edit/codeaction/codeaction.c (orchestrator capacity widened to MAX_VARIANTS, multi-action emit loop)
    - src/lsp/facade/edit/codeaction/resolve.c (handler dispatch into stack-local variants[]; variant_idx slot pick)
    - src/lsp/facade/edit/codeaction/quickfix_undefined_var.c (mechanical signature update; *out_n = 1)
    - src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c (mechanical update)
    - src/lsp/facade/edit/codeaction/quickfix_missing_return.c (mechanical update + drop local derive_body_indent + consume codeaction_indent.h)
    - src/lsp/facade/edit/codeaction/quickfix_unused_import.c (mechanical update)
    - src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c (mechanical update)
    - src/lsp/server/handlers_edit.c (variant_idx encode + decode; command:{} branch; multi-edit edits[] branch; preserved single-edit fast path)
    - CMakeLists.txt (ironls links codeaction_indent.c)
    - tests/unit/CMakeLists.txt (test_codeaction_registry binary links codeaction_indent.c)
    - tests/lsp/unit/CMakeLists.txt (3 new add_executable blocks + 2 source-list registrations of codeaction_indent.c)
    - tests/lsp/fmt/CMakeLists.txt (test_fmt_quickfix_clean links codeaction_indent.c)
    - tests/unit/test_codeaction_registry.c (5 shape tests + 1 refusal test + 5 NULL-guard cases updated to new signature)
    - tests/lsp/fmt/quickfix/test_fmt_quickfix_clean.c (parametrized fmt-quickfix-clean fixture loop updated to new signature)

key-decisions:
  - "D-11 implemented: IronLsp_QuickfixFn signature widened from (out) to (out_arr, out_cap, out_n) with ILSP_QUICKFIX_MAX_VARIANTS = 2. Refusal protocol switches from edit_new_text == NULL to *out_n == 0 (Pitfall 2)."
  - "D-12 implemented: all 5 existing handlers updated mechanically; each still emits exactly 1 action on success and *out_n = 0 on refusal. Regression-tested via existing Phase 4 codeAction unit tests."
  - "D-13 implemented: codeaction.c orchestrator capacity widened to walk_diags.count * MAX_VARIANTS + 1; per-emission stamps data_variant_idx = (int)v."
  - "D-14 implemented: IronLsp_CodeAction.command_title / command_id / command_args / command_args_n added. Mutually exclusive with edit_* per LSP 3.17 §CodeAction."
  - "D-15 implemented: code_action_to_json wire serializer emits `command:{title, command, arguments[]}` envelope when command_id != NULL; OMITS `edit` field per LSP 3.17 semantics."
  - "D-23 implemented: derive_body_indent lifted from quickfix_missing_return.c into shared codeaction_indent.{c,h} as ilsp_codeaction_derive_body_indent. Recommended path per CONTEXT.md Claude's Discretion §1; QF-02 + QF-03 (Plan 12-02 / 12-03) consume the same source-of-truth."
  - "D-27/D-28 implemented (extend-now path per CONTEXT.md Claude's Discretion §2): IronLsp_CodeAction grows edit_text_edits[] + edit_text_edits_n; wire serializer emits N-entry edits[] inside documentChanges/changes."
  - "D-31 implemented (explicit field path per RESEARCH Open Item #5; high-byte trick rejected): data_variant_idx is a separate int field in both struct + wire data object. Legacy clients sending no key default to 0 (Pitfall 8)."
  - "D-37 implemented: 3 new test binaries follow Phase 11 test_v3_patch_predicate template verbatim with phase-m1-invariant + phase-12-invariant dual labels. CTest TIMEOUT 60."
  - "D-38 preserved: zero touches to src/parser/, src/analyzer/, src/diagnostics/, src/hir/, src/lir/, src/runtime/, src/stdlib/. Parity gate (HARD-24, test_parity_ironc_lsp + _fmt) green throughout."
  - "T-12-01-01 mitigated: resolve.c bounds-checks data_variant_idx (negative or >= variant_n returns refusal cleanly)."
  - "T-12-01-03 mitigated: cap = walk_diags.count * MAX_VARIANTS + 1 with MAX_VARIANTS = 2 keeps worst-case allocation bounded under Phase 1 HARD-06 budget."

patterns-established:
  - "Multi-action quickfix emit: handlers receive a caller-provided buffer (out_arr) of capacity out_cap, write 0..N actions, set *out_n. QF-04/QF-05 (Plan 12-03) emit 2 actions per diagnostic; the 5 existing handlers and QF-01..02 stay single-action."
  - "Three mutually-exclusive action shapes on IronLsp_CodeAction: command-style (command_id != NULL), multi-edit (edit_text_edits_n > 0), legacy single-edit (edit_new_text != NULL). Wire serializer picks the matching JSON envelope in priority order."
  - "Per-action variant_idx lazy-resolve: codeaction.c stamps a quad on each emission (file_version, code, diagnostic_idx, variant_idx); resolve decoder reads all 4 keys (variant_idx defaults to 0 for legacy clients); facade dispatches to variants[data_variant_idx]."
  - "Wave 0 stub binary pattern: TEST_IGNORE_MESSAGE under phase-m1-invariant + phase-N-invariant dual labels, verbatim Phase 11 template; downstream plans flip to real assertions by upgrading link sets and writing test bodies."
  - "Single-helper module shape (codeaction_indent.{c,h} mirroring nav/visibility.{c,h}): forward-declare consumed structs in the header; pure NULL-safe implementation with no allocation/I/O/globals; safe for concurrent request threads per CLAUDE.md concurrency contract."

requirements-completed: []  # Plan 12-01 owns no requirement IDs directly — KW-01..03 + QF-01..05 are claimed by Plans 12-02 / 12-03. This plan delivers their substrate.

# Metrics
duration: ~30min
completed: 2026-04-29
---

# Phase 12 Plan 12-01: Phase-12 substrate — multi-action quickfix shape, body-indent helper lift, Wave 0 stubs Summary

**Multi-action quickfix substrate landed: IronLsp_QuickfixFn signature widened to (out_arr, out_cap, out_n); IronLsp_CodeAction grew command_* / edit_text_edits[] / data_variant_idx fields; wire serializer emits `command:{}` and N-entry `edits[]` envelopes; legacy single-edit fast path preserved verbatim. Three Wave 0 stub binaries wired under phase-12-invariant for Plans 12-02/12-03 to flip to real assertions.**

## Performance

- **Duration:** ~30 minutes
- **Started:** 2026-04-29T00:56:44Z (worktree base 9788707)
- **Completed:** 2026-04-29T01:13:34Z
- **Tasks:** 6/6 (atomic per-task commits)
- **Files modified:** 17 (5 created + 12 modified)

## Accomplishments

- **Multi-action quickfix substrate:** IronLsp_QuickfixFn signature widened from `(out)` to `(out_arr, out_cap, out_n)`; refusal switches from `edit_new_text == NULL` to `*out_n == 0`. ILSP_QUICKFIX_MAX_VARIANTS = 2 supports QF-04/QF-05's two-action requirement in Plan 12-03 without further infra changes.
- **Wire serializer extensions:** `code_action_to_json` emits `command:{title, command, arguments[]}` (D-15) and N-entry `edits[]` arrays (D-27) with full `documentChanges` / `changes` fallback parity. `variant_idx` round-trips through the data object on both encode + decode (D-31). Legacy single-edit fast path preserved verbatim — the 5 existing handlers + organizeImports keep their wire shape byte-for-byte.
- **Body-indent helper lift:** `derive_body_indent` moved from `quickfix_missing_return.c` into shared `codeaction_indent.{c,h}` as `ilsp_codeaction_derive_body_indent`. QF-02 + QF-03 (Plans 12-02 / 12-03) will consume the same source-of-truth.
- **Wave 0 test scaffolding:** 3 stub binaries (`test_v3_quickfix_corpus`, `test_v3_keyword_filter`, `test_v3_keyword_drift`) registered under `phase-m1-invariant;phase-12-invariant` with TIMEOUT 60. Each reports as Pass with 1 IGNORED test under TEST_IGNORE_MESSAGE per VALIDATION.md "Wave 0 Requirements".
- **Parity gate preserved (D-38):** zero touches to `src/parser/`, `src/analyzer/`, `src/diagnostics/`, `src/hir/`, `src/lir/`, `src/runtime/`, `src/stdlib/`. `test_parity_ironc_lsp` + `test_parity_ironc_lsp_fmt` green throughout.

## Task Commits

Each task was committed atomically (--no-verify due to parallel-executor worktree):

1. **Task 1: Wave 0 stubs + codeaction_indent helper lift** — `7675e85` (chore)
2. **Task 2: Widen IronLsp_QuickfixFn + IronLsp_CodeAction in registry.h** — `2d17eee` (feat)
3. **Task 3: Update orchestrator + resolver to multi-action signature; thread data_variant_idx** — `084dd1b` (feat)
4. **Task 4: Mechanical signature update across 5 handlers + missing_return consumes codeaction_indent.h** — `55479d3` (feat)
5. **Task 5: Extend handlers_edit.c wire serializer (command{} + multi-edit + variant_idx)** — `c6c1b91` (feat)
6. **Task 6: Full-suite regression sweep + plan close** — (this commit; SUMMARY.md only)

## Files Created/Modified

### Created (5)

- `src/lsp/facade/edit/codeaction/codeaction_indent.h` — single-helper module header; declares `ilsp_codeaction_derive_body_indent` under `extern "C"`. Forward-declares `struct IronLsp_Document;`. Header guard `IRON_LSP_FACADE_EDIT_CODEACTION_CODEACTION_INDENT_H`.
- `src/lsp/facade/edit/codeaction/codeaction_indent.c` — body lifted verbatim from `quickfix_missing_return.c:40-72`. NULL-safe, no allocation, no globals.
- `tests/lsp/unit/test_v3_quickfix_corpus.c` — Wave 0 stub. Plan 12-03 wires fixtures + assertions.
- `tests/lsp/unit/test_v3_keyword_filter.c` — Wave 0 stub. Plan 12-02 implements `keyword_filter` predicate matrix.
- `tests/lsp/unit/test_v3_keyword_drift.c` — Wave 0 stub. Plan 12-03 wires drift assertions.

### Modified (12)

- `src/lsp/facade/edit/codeaction/registry.h` — `ILSP_QUICKFIX_MAX_VARIANTS = 2`; `IronLsp_TextEdit` struct; struct grew with `command_*`, `edit_text_edits[]`, `data_variant_idx`; typedef takes `(out_arr, out_cap, out_n)`; 5 extern decls updated.
- `src/lsp/facade/edit/codeaction/registry.c` — no functional change; the const-sorted table still has 5 rows, all referencing the same handler symbols (with the new typedef).
- `src/lsp/facade/edit/codeaction/codeaction.h` — `ilsp_facade_code_action_resolve` gains `int data_variant_idx` parameter.
- `src/lsp/facade/edit/codeaction/codeaction.c` — orchestrator capacity multiplied by MAX_VARIANTS; inner loop calls handler with new signature into stack-local `variants[2]`; per-emission stamps the 4-tuple of `data_*`.
- `src/lsp/facade/edit/codeaction/resolve.c` — handler dispatch into stack-local variants buffer; `data_variant_idx` bounds-checked; `*out = variants[data_variant_idx]` then re-stamps `data_*` fields.
- `src/lsp/facade/edit/codeaction/quickfix_undefined_var.c` — mechanical signature update.
- `src/lsp/facade/edit/codeaction/quickfix_type_mismatch_literal.c` — mechanical signature update.
- `src/lsp/facade/edit/codeaction/quickfix_missing_return.c` — mechanical signature update + dropped local `static derive_body_indent` (lines 40-72) + added `#include "lsp/facade/edit/codeaction/codeaction_indent.h"` + call site uses lifted symbol.
- `src/lsp/facade/edit/codeaction/quickfix_unused_import.c` — mechanical signature update.
- `src/lsp/facade/edit/codeaction/quickfix_redundant_cast.c` — mechanical signature update.
- `src/lsp/server/handlers_edit.c` — `code_action_to_json` emits `variant_idx` data key (encoder); 3-branch priority cascade (command / multi-edit / legacy single-edit); resolve handler decodes `variant_idx` (decoder, defaults to 0 for legacy clients); resolve emit-merge block extended to the same 3-branch cascade plus `null` for stale.
- `CMakeLists.txt` — ironls SRC list registers `codeaction_indent.c`.
- `tests/unit/CMakeLists.txt` — `test_codeaction_registry` binary links `codeaction_indent.c`.
- `tests/lsp/unit/CMakeLists.txt` — 3 new `add_executable` blocks for the Wave 0 stubs (full LSP server + transport stack for corpus binary; minimal stack for filter; single-source for drift). 2 source-list locations (`_LSP_PLAN03_SERVER_SRC` and `test_workspace_diagnostic`) gain `codeaction_indent.c`.
- `tests/lsp/fmt/CMakeLists.txt` — `test_fmt_quickfix_clean` links `codeaction_indent.c`.
- `tests/unit/test_codeaction_registry.c` — 5 shape tests + 1 refusal test + 5 NULL-guard tests updated to new signature; refusal-detection switches from `edit_new_text == NULL` to `out_n == 0` (Pitfall 2 propagation).
- `tests/lsp/fmt/quickfix/test_fmt_quickfix_clean.c` — parametrized fmt-quickfix-clean fixture loop updated to new signature plus asserts `out_n == 1` before reading the slot.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Test source lists missing codeaction_indent.c**
- **Found during:** Task 4 (mechanical handler update + indent consume)
- **Issue:** After lifting `derive_body_indent` into a new TU, the ironls binary, `test_codeaction_registry`, `test_fmt_quickfix_clean`, and the test_workspace_diagnostic binary (via `_LSP_PLAN03_SERVER_SRC`) all failed to link with `undefined reference to ilsp_codeaction_derive_body_indent`.
- **Fix:** Added `codeaction_indent.c` to root `CMakeLists.txt` ironls SRC list, `tests/unit/CMakeLists.txt` `test_codeaction_registry` SRC list, `tests/lsp/fmt/CMakeLists.txt` `test_fmt_quickfix_clean` SRC list, and BOTH source-list locations in `tests/lsp/unit/CMakeLists.txt` (`_LSP_PLAN03_SERVER_SRC` and the standalone `test_workspace_diagnostic` block).
- **Files modified:** 4 CMakeLists.txt files
- **Commit:** `55479d3`

**2. [Rule 3 - Blocking] Existing direct-call tests break under typedef shift**
- **Found during:** Task 4 (mechanical handler update)
- **Issue:** `tests/unit/test_codeaction_registry.c` and `tests/lsp/fmt/quickfix/test_fmt_quickfix_clean.c` call quickfix handlers directly via the public registry API. After the typedef shift these tests no longer compiled.
- **Fix:** 11 call sites updated to new signature. Refusal-detection in NULL-guard tests switches from `out.edit_new_text == NULL` to explicit `out_n == 0` assertion (Pitfall 2 propagation: multi-edit / command-style actions leave `edit_new_text` NULL by design, so the old assertion would no longer be a refusal indicator going forward — even though Plan 12-01's existing handlers all stay single-action, the test contract must reflect the new protocol).
- **Files modified:** `tests/unit/test_codeaction_registry.c`, `tests/lsp/fmt/quickfix/test_fmt_quickfix_clean.c`
- **Commit:** `55479d3`

**3. [Rule 3 - Blocking] test_v3_quickfix_corpus link failure on yyjson + transport symbols**
- **Found during:** Task 1 first build attempt
- **Issue:** Initial CMake wiring used only `${_LSP_PLAN03_SERVER_SRC}` for `test_v3_quickfix_corpus`. handlers_*.c reach into the transport writer queue + yyjson serializer; these symbols are in `_LSP_PLAN03_TRANSPORT_SRC`. Linker reported ~30 undefined references.
- **Fix:** Added `${_LSP_PLAN03_TRANSPORT_SRC}` to the corpus binary's source list (matches the lifecycle/capabilities binary template).
- **Files modified:** `tests/lsp/unit/CMakeLists.txt`
- **Commit:** Folded into `7675e85` (Task 1 commit) before final build.

### Out-of-scope discoveries (logged for tracking, not fixed)

- **`test_string_intern_race` linker fails on this dev system** — `cannot find /usr/lib64/libtsan.so.0.0.0`. Pre-existing environment issue (TSAN runtime not installed), label is `tsan` not `phase-m1-invariant`. Not blocking Plan 12-01 gate criteria. Should be tracked as a sysadmin/CI concern; not Phase 12's responsibility.

## Verification

### Build (D-38 / parity-gate-safe)

```
cmake --build build --target ironls
```

- **Exit code:** 0
- **Warnings:** 0 (under `-Wall -Wextra -Werror -Wpedantic -Werror=switch-enum`)
- **Files compiled:** 90+ TUs

### CTest invariant sweep

```
ctest --test-dir build -L phase-m1-invariant       # 27/27 passed (incl. 3 Wave 0 stubs)
ctest --test-dir build -L phase-12-invariant       # 3/3 passed
ctest --test-dir build -R test_parity_ironc_lsp    # 3/3 passed (HARD-24 / D-38 preserved)
ctest --test-dir build -R test_codeaction          # 1/1 passed (5 existing quickfix shape tests + 1 refusal + NULL-guard)
ctest --test-dir build -R test_organize_imports    # 1/1 passed (regression: organizeImports sentinel path unaffected)
ctest --test-dir build -R test_fmt_quickfix        # 1/1 passed (Phase 5 D-07 fmt-quickfix-clean gate)
```

Full m1..m6 invariant set: **121/121 passed** in 49.25 sec.

### Patch-shape grep gates (Plan 12-01 success criteria)

```
$ grep -c "ILSP_QUICKFIX_MAX_VARIANTS" src/lsp/facade/edit/codeaction/registry.h
3   # macro + 2 doc references in headers/typedefs

$ grep -q "ilsp_codeaction_derive_body_indent" src/lsp/facade/edit/codeaction/codeaction_indent.h && echo OK
OK

$ ! grep -q "static uint32_t derive_body_indent" src/lsp/facade/edit/codeaction/quickfix_missing_return.c && echo OK
OK   # local helper successfully removed

$ grep -c 'yyjson_mut_obj_add_int.*"variant_idx"' src/lsp/server/handlers_edit.c
1   # encoder side (data object)

$ grep -c 'yyjson_obj_get.*"variant_idx"' src/lsp/server/handlers_edit.c
1   # decoder side (resolve request)
```

## Patterns Followed

- **PATTERNS.md "Pattern: Single-helper module shape (predicate/utility module)"** — `codeaction_indent.{c,h}` mirrors `nav/visibility.{c,h}` verbatim: phase provenance comment + extern "C" + forward-declared consumed struct + NULL-safe pure-read body.
- **PATTERNS.md "Pattern: Quickfix handler TU shape"** — 5 mechanical updates applied identical sentinel-preamble + `out_arr[0]` field-rewrite + `*out_n = 1` terminator pattern.
- **PATTERNS.md "registry.h (signature widening)"** — verbatim shape adopted: macro defined before typedef; struct fields appended at the end (legacy zero-init produces inert action); 5 extern decls updated.
- **PATTERNS.md "src/lsp/server/handlers_edit.c (wire serializer)"** — verbatim 3-branch priority cascade (command_id → edit_text_edits → edit_new_text → null/refusal).
- **RESEARCH.md Pitfall 1, 2, 8 mitigations applied:**
    - Pitfall 1 (legacy zero-init): new fields appended at struct end so unset values default to 0/NULL.
    - Pitfall 2 (refusal protocol): switched from `edit_new_text == NULL` to `*out_n == 0` everywhere; existing tests updated to match.
    - Pitfall 8 (legacy clients): decoder defaults `vidx = 0` when the data object lacks the key.

## Confirmed: Parity-Gate Safety (D-38)

`git diff 9788707..HEAD -- src/parser/ src/analyzer/ src/diagnostics/ src/hir/ src/lir/ src/runtime/ src/stdlib/` shows zero changes. The compiler frontend / middle-end / backend / runtime / stdlib are untouched. `test_parity_ironc_lsp` + `test_parity_ironc_lsp_fmt` green throughout the 5 commits.

## Self-Check: PASSED

All claimed file paths exist:
- `src/lsp/facade/edit/codeaction/codeaction_indent.h` — FOUND
- `src/lsp/facade/edit/codeaction/codeaction_indent.c` — FOUND
- `tests/lsp/unit/test_v3_quickfix_corpus.c` — FOUND
- `tests/lsp/unit/test_v3_keyword_filter.c` — FOUND
- `tests/lsp/unit/test_v3_keyword_drift.c` — FOUND

All claimed commit hashes exist:
- `7675e85` (Task 1) — FOUND
- `2d17eee` (Task 2) — FOUND
- `084dd1b` (Task 3) — FOUND
- `55479d3` (Task 4) — FOUND
- `c6c1b91` (Task 5) — FOUND

Plan 12-01 is gate for Plan 12-02. Substrate is now in place for KW-01..03 + QF-01..05.
