# LSP Memory Model Adaptation — Phase 34 Closeout

**Phase:** 34 — `lsp-adaptation`
**Status:** COMPLETE (5 plans landed; phase34-invariant 10/10 GREEN; HARD-24 parity 5/5 GREEN; CORE-22 invariant preserved end-to-end)
**Completed:** 2026-05-31

This document is the canonical reference for the Phase 34 outcome: the
hover annotation block that surfaces every v4 memory-model annotation,
the keyword-visibility predicates that gate the 10 new v3/v4 keywords,
the `defer free <binding>` snippet auto-discovered from the cursor's
enclosing function body, the five memory-model quickfixes (LSP-06..10),
the 800-range diagnostic namespace reservation, and the four documented
residuals that future phases will pick up.

This is the **first** phase to ship source edits under `src/lsp/facade/`
since Phase 17 began the v4 memory-model push. Phases 17-33 attached
every annotation Phase 34 surfaces; this phase wires the LSP-side
consumer paths.

---

## 1. Scope recap

Phase 34 added the LSP-side surface for the v4 memory-model annotations
introduced in Phases 17-33. The high-level deliverables, indexed by the
plan that owned each:

| Plan  | Wave | Deliverable                                                                  |
| ----- | ---- | ---------------------------------------------------------------------------- |
| 34-01 | 0    | Fixture corpus (41 files), `phase34-invariant` CTest label, 800-range reservation, CORE-22 structural invariant test |
| 34-02 | 1A   | Hover memory-model annotation block (LSP-01, LSP-02)                         |
| 34-03 | 1B   | Keyword visibility for 10 new v3/v4 keywords + `defer free <binding>` snippet (LSP-03, LSP-04) |
| 34-04 | 2    | 5 new memory-model quickfix handlers (LSP-06..10)                            |
| 34-05 | 3    | Final sweep + this doc + phase closeout summary (LSP-11, LSP-12, LSP-13)     |

Every plan honoured the **CORE-22** discipline: a single
`iron_analyze_buffer(` call site under `src/lsp/`, in
`src/lsp/facade/compile.c:57`. All new surface code reads from the
already-resolved `Iron_Symbol` / `Iron_Type` produced by that single
call. The structural `test_core22_single_analyze` ctest invariant
(registered Plan 34-01) greps for `iron_analyze_buffer(` (call form,
trailing open-paren) and asserts exactly one match.

Every plan honoured the **HARD-24** discipline: the five parity tests
(`test_parity_ironc_lsp`, `_fmt`, `_suggestions`,
`test_parity_v3_print_fixed_point`, `test_lexer_doc_comment_parity`)
were GREEN before each task started and GREEN after each task
committed. Hover output, completion output, and quickfix output flow
through the LSP-only render paths; they do not contribute to the
ironc-vs-LSP diagnostic-text byte-for-byte comparisons that HARD-24
verifies.

---

## 2. Hover annotation block (LSP-01, LSP-02)

### 2.1 Layout

Hover output for any `val` / `var` binding, function param, method
receiver, or function signature consists of two stacked sections:

1. The **fenced signature block** (existing Phase 3 surface): one
   fenced code block with the canonical signature on a single line,
   e.g. `val buffer: Buffer`, `pub readonly func Reader.describe(self: Reader) -> Int`.
2. A new **annotation block** (Phase 34) immediately beneath, listing
   non-default memory-model fields. Fields appear in fixed order:
   ```
   policy: heap        -- omitted when default = stack
   regime: unchecked   -- omitted when default = checked
   readonly: yes       -- omitted when default = no
   nocopy: yes         -- omitted when default = no
   ```
   When every field is at its default value (bare primitive bindings),
   the annotation block is empty. The `hover_policy_stack.expected`
   fixture pins this empty-block behaviour.

### 2.2 Source of truth

Each field has a single resolved source:

| Field      | Source                                                                                          |
| ---------- | ----------------------------------------------------------------------------------------------- |
| `policy`   | `Iron_ValDecl.init` kind (`IRON_NODE_HEAP` / `IRON_NODE_RC` / `IRON_NODE_WEAK_RC_NULL`); falls back to `sym->type` / `declared_type` kind (`IRON_TYPE_RC` / `IRON_TYPE_WEAK_RC`). |
| `regime`   | `effective_type(decl, sym)->ptr.is_unchecked` for `IRON_TYPE_PTR`.                              |
| `readonly` | `Iron_FuncDecl.is_readonly` / `Iron_MethodDecl.is_readonly`; falls back to `type->is_readonly_compatible` for transitive-readonly bindings. |
| `nocopy`   | `effective_type(decl, sym)->object.decl->is_nocopy` for `IRON_TYPE_OBJECT`.                     |

The four `derive_*` static helpers live in `src/lsp/facade/hover.c`:

- `derive_policy(decl, sym)` at `hover.c:395`
- `derive_regime(decl, sym)` at `hover.c:425`
- `derive_readonly(decl, sym)` at `hover.c:439`
- `derive_nocopy(decl, sym)` at `hover.c:455`

The `effective_type(decl, sym)` bridge at `hover.c:380` returns the
resolved `Iron_Type *` from either `sym->type` (when the cursor
resolved to an `IDENT`) or directly from `Iron_ValDecl.declared_type`
/ `Iron_VarDecl.declared_type` (when the cursor resolved to a decl
node after body descent). All four `derive_*` helpers route through
this single bridge so the annotation pipeline never has to special-case
the IDENT-vs-decl resolution path.

The annotation block is appended at `hover.c:959-1002`, immediately
after the fenced signature block closes and before the doc-comment
block (when one exists). The appender calls each `derive_*` helper
once, sb-appends only the non-default fields, and emits a trailing
newline so the markdown renderer treats the block as a paragraph.

### 2.3 Hover-only body descent

The shared `ilsp_nav_node_at` (see `src/lsp/facade/nav/node_at.c:130-133`
author note) deliberately stops at `FUNC_DECL` / `METHOD_DECL`
boundaries — definition, references, and declaration endpoints all
want the enclosing decl as their finest-grain answer. **Hover does
not**: every binding the LSP-01/LSP-02 surface needs to annotate
(`val buffer = heap T(...)`, `val raw: *unchecked Int = ...`) lives
inside a function body.

Plan 34-02 added `hover_descend_into_func_body` at `hover.c:683` and a
`walk_node_for_hover` recursive walker at `hover.c:627-707`. When
`ilsp_nav_node_at` returns a coarse-grain FUNC_DECL/METHOD_DECL answer
for a cursor inside its body, the descent walker re-walks the block
+ every `Iron_*Stmt` with children and returns the smallest covering
`VAL_DECL` / `VAR_DECL` / `PARAM` / `IDENT`. The walker is scoped to
hover.c only — other nav endpoints keep their decl-level behaviour.

### 2.4 Signature derivation

`signature_val` / `signature_var` (`hover.c:307-345`) take the
resolved `Iron_Symbol *` in addition to the decl, and route through
`render_decl_type` which prefers `iron_type_to_string` (the
analyzer's full renderer used by ironc for diagnostics) over the
older `render_type_ann` (Path-style minimal renderer). The flip is
load-bearing for the annotation block: `render_type_ann` truncated
`*unchecked Int` to `*` and `rc Point` to `Point`, which would have
broken the `hover_regime_unchecked` and `hover_policy_rc` fixtures.

---

## 3. Completion keyword visibility (LSP-03)

`src/lsp/facade/edit/complete/keyword_filter.c::ilsp_keyword_visible_at`
gains 10 new `strcmp` arms (lines 283-356), one per v3/v4 keyword.
Each arm is paired with a "deny everything else" sentinel in the
default rejection block (lines 363-372) so the 38 pre-v3 keywords keep
their byte-exact Phase 4 EDIT-06 behaviour.

| Keyword     | Visible when                                                          | Hidden when                                              |
| ----------- | --------------------------------------------------------------------- | -------------------------------------------------------- |
| `heap`      | `ctx == TYPE_POSITION` OR rhs-of-`=` on line                          | MEMBER/IMPORT/QUALIFIED; pure STATEMENT_HEAD             |
| `rc`        | (same as `heap`)                                                      | (same as `heap`)                                         |
| `weak`      | (same as `heap`)                                                      | (same as `heap`)                                         |
| `unchecked` | `ctx == TYPE_POSITION` OR `*` precedes on line OR rhs-of-`=`          | MEMBER/IMPORT/QUALIFIED; pure STATEMENT_HEAD without `*` |
| `defer`     | `ctx == STATEMENT_HEAD`                                               | every other context                                      |
| `drop`      | enclosing `Iron_ObjectDecl` body AND decl-head text-position          | function bodies; module top-level                        |
| `copy`      | (same as `drop`)                                                      | (same as `drop`)                                         |
| `nocopy`    | `ctx == STATEMENT_HEAD` AND decl-head text-position                   | MEMBER/IMPORT/QUALIFIED/TYPE; expression positions       |
| `leak`      | `ctx == EXPR_HEAD` OR `ctx == STATEMENT_HEAD`                         | TYPE_POSITION; MEMBER/IMPORT/QUALIFIED                   |
| `in`        | `for` token precedes on the same line                                 | every other position                                     |

Three byte-walk helpers (`kw_rhs_of_assign_on_line`,
`kw_star_precedes_on_line`, `kw_for_precedes_on_line`) handle the
positions where the structured context classifier has insufficient
resolution. They are pure reads with bounded loops; no allocator
touched.

---

## 4. `defer free <binding>` snippet (LSP-04)

### 4.1 Backward-scan module

`src/lsp/facade/edit/complete/defer_free.{c,h}` ships a standalone
module rather than living as a static inside `complete.c`. The split
is required because the Plan 34-03 test driver
(`test_complete_defer_free_snippet`) exercises the AST walker against
synthetic programs and would otherwise have to drag the full
transport/server stack into the test binary. The split also mirrors
the Phase 12 Plan 12-02 precedent (`keyword_filter.{c,h}` extracted
from `buckets.c` for the same reason).

The public entry point is:

```c
size_t ilsp_collect_recent_heap_rc_bindings(
    const Iron_Program *program,
    uint32_t            cursor_line_1,
    Iron_Arena         *arena,
    const char        **out_names,
    size_t              out_cap);
```

`find_enclosing_function_body` locates the `Iron_FuncDecl` /
`Iron_MethodDecl` whose body span covers `cursor_line_1` and returns
its `Iron_Block *`. `collect_from_block` reverse-iterates the body's
statements and dispatches per kind: `Iron_ValDecl` / `Iron_VarDecl`
whose `init->kind` is `IRON_NODE_HEAP` or `IRON_NODE_RC`, and whose
span strictly precedes the cursor line, contribute one entry each.
`Iron_IfStmt`, `Iron_WhileStmt`, `Iron_ForStmt` recursive blocks are
walked transparently so a heap binding inside an `if` branch surfaces
when the cursor sits below. `ILSP_DEFER_FREE_MAX_CANDIDATES = 5`
caps the output; reverse-iteration guarantees the most-recent binding
lands at `out_names[0]`.

### 4.2 Snippet renderer

`src/lsp/facade/edit/complete/snippet.h` gains the
`ILSP_SNIPPET_DEFER_FREE` enum variant (line 48). The
`render_defer_free` static at `snippet.c:186` emits the LSP 3.17
Snippet Syntax form `defer free ${1:<binding>}$0` — `$1` as the
single tabstop so the user can tab-cycle and override the
auto-discovered name. The binding name flows through
`sb_append_placeholder` → `sb_append_escaped`, which escapes `$`,
`}`, and `\` per the LSP Snippet Syntax Appendix. This is the
**PITFALL D** guard against hostile identifiers like `${USER}\bad}`
injecting shell-variable lookups into the rendered snippet body.

### 4.3 Orchestrator hook

`emit_defer_free_snippets` is the new entry point in
`src/lsp/facade/edit/complete/complete.c` (lines 388-410). It gates on
three conditions:

1. `server->client_supports_snippet` is true (no snippet noise on
   plain-text-only clients).
2. `ctx == ILSP_CCTX_STATEMENT_HEAD`.
3. `has_prefix_of_defer_free(query_prefix)` — empty prefix OK,
   otherwise the typed-so-far text must be a prefix of `"defer free"`.

When all three hold, it calls
`ilsp_collect_recent_heap_rc_bindings` once, appends one
`IronLsp_CompletionCandidate` per returned binding via stb_ds
`arrput`, sets `kind=14` (KEYWORD) so the snippets cluster with the
bare `defer` candidate, and assigns `fuzzy_score = 1000.0 - i` so the
most-recent binding outranks generic-keyword scores within the same
bucket.

`attach_auto_import_and_snippets` gains a single-line early-continue
guard for candidates with `insert_text_format == 2` (Snippet). Without
the guard, the function's per-kind switch falls through to a default
arm that resets `insert_text_format = 1` (PlainText), and the
pre-rendered snippet body becomes a literal user-visible string.

---

## 5. 800-range diagnostic codes (LSP-05)

### 5.1 Reservation

`src/diagnostics/diagnostics.h:82-105` carries the
**IRON_ERR_RANGES** comment block: the canonical 1-899 namespace map
extended with the Phase 34 sub-allocation:

```
800 – 809 : lifecycle-policy errors        (missing val/var, forgotten free, unused-with-allocation)
810 – 819 : regime errors                  (&rc_value, unchecked-in-checked-context)
820 – 829 : readonly/purity violations     (readonly fn touching heap/rc/weak rc)
830 – 839 : drop/copy/nocopy violations
840 – 899 : reserved for follow-up phases
```

Only one stable symbol ships from this range so far:

```c
#define IRON_ERR_READONLY_MEMORY            820
```

This is the matcher the LSP-10 quickfix (`quickfix_readonly_memory.c`)
keys against. Other ranges carry placeholder comments — future plans
allocate codes inside them on demand without re-litigating the
sub-range map.

### 5.2 Discipline

`diagnostics.h:624` carries the critical inline note: **all emit
sites for 800-range codes MUST live inside `iron_compiler` (never
under `src/lsp/`)**. A `src/lsp/` code path that calls
`iron_diag_emit(..., IRON_ERR_*, ...)` for any 800-range code would
inject diagnostics into the LSP-side `Iron_DiagList` that ironc
doesn't emit, breaking HARD-24 byte-for-byte parity instantly. The
quickfix consumer-only invariant is structurally enforced — see §6.

---

## 6. Memory-model quickfixes (LSP-06..10)

Five new handler TUs land under
`src/lsp/facade/edit/codeaction/`, registered in
`registry.c` in ASC-sorted order. The 11-entry table from Phase 12
grows to 16 entries:

| Code | Symbol                            | Handler                                  | LSP-XX |
| ---- | --------------------------------- | ---------------------------------------- | ------ |
| 176  | `IRON_ERR_MISSING_VAL_VAR`        | `ilsp_quickfix_missing_val_var`          | LSP-06 |
| 296  | `IRON_ERR_PTR_AMP_ON_RC`          | `ilsp_quickfix_amp_on_rc`                | LSP-09 |
| 606  | `IRON_WARN_FORGOTTEN_FREE`        | `ilsp_quickfix_forgotten_free`           | LSP-07 |
| 613  | `IRON_WARN_UNUSED_VAR`            | `ilsp_quickfix_unused_var_alloc`         | LSP-08 |
| 820  | `IRON_ERR_READONLY_MEMORY`        | `ilsp_quickfix_readonly_memory`          | LSP-10 |

Four are single-action (one quickfix per error code, no ambiguity);
LSP-10 is the only two-variant quickfix in scope ("Remove 'readonly'"
+ "Extract mutating block into helper"). Both LSP-10 variants set
`is_preferred = false` per the D-31 semantic-ambiguity convention
(never auto-pick when the fix is genuinely ambiguous).

### 6.1 Consumer-only invariant

**No quickfix handler under `src/lsp/facade/edit/codeaction/` ever
calls `iron_diag_emit`.** Quickfixes consume the already-loaded
`Iron_Program` returned by the single `iron_analyze_buffer` call
(via `ilsp_facade_compile_for_nav`) and emit `WorkspaceEdit` →
`TextEdit` payloads. The invariant is verified by a grep gate as
part of phase closeout:

```bash
grep -rn 'iron_diag_emit' src/lsp/facade/edit/codeaction/   # returns 0 lines
```

### 6.2 Test pattern

Each handler ships with a per-fixture Unity driver under
`tests/lsp/quickfix/`. The shared `quickfix_fixture_runner.h`
provides slurp/parse/diff helpers (`qf34_parse_single`,
`qf34_parse_multi`, `qf34_mk_diag`) with `__attribute__((unused))`
so each driver uses a subset without triggering
`-Werror=unused-function`.

Drivers **synthesize** the triggering `Iron_Diagnostic` at the
fixture's known coordinates rather than driving the full compile
pipeline. This decouples LSP-side handler-correctness verification
from compiler-side emit-site readiness. See §8.1 for the
documented residual that motivates this pattern.

---

## 7. Acceptance evidence

All gates ran on **silvaserver podman (8GB cap, image
`iron-lsp-build:latest`)** per the project's remote-execution
discipline.

| Gate                                                  | Result                                |
| ----------------------------------------------------- | ------------------------------------- |
| `ctest -L phase34-invariant`                          | **10/10 PASSED**                      |
| `ctest -R "test_parity_(ironc_lsp\|v3_print_fixed_point)\|test_lexer_doc_comment_parity"` | **5/5 GREEN** (HARD-24) |
| `ctest -R test_core22_single_analyze`                 | **GREEN**                             |
| `ctest -R test_codeaction_registry`                   | **GREEN** (16 ASC-sorted entries)     |
| `ctest -R test_hover_memory_model`                    | **7/7 GREEN** (Plan 34-02)            |
| `ctest -R test_complete_v4_keywords`                  | **10/10 GREEN** (Plan 34-03)          |
| `ctest -R test_complete_defer_free_snippet`           | **7/7 GREEN** (Plan 34-03)            |
| `ctest -L quickfix`                                   | **5/5 GREEN** (Plan 34-04)            |
| `grep -rn 'iron_analyze_buffer(' src/lsp/`            | **1 line** (`src/lsp/facade/compile.c:57`) |
| `grep -rn 'iron_diag_emit' src/lsp/facade/edit/codeaction/` | **0 lines**                     |

The `phase34-invariant` label aggregates the entire phase's CTest
surface:

```
lsp_publish_diag_820                         PASS
test_core22_single_analyze                   PASS  (structural invariant)
test_hover_memory_model                      PASS  (Plan 34-02; 7 fixtures inside)
test_quickfix_lsp_06_missing_val_var         PASS
test_quickfix_lsp_07_forgotten_free          PASS
test_quickfix_lsp_08_unused_var_alloc        PASS
test_quickfix_lsp_09_amp_rc                  PASS
test_quickfix_lsp_10_readonly_memory         PASS
test_complete_v4_keywords                    PASS  (10 RUN_TEST entries inside)
test_complete_defer_free_snippet             PASS  (7 RUN_TEST entries inside)
```

---

## 8. Documented residuals (4)

Each residual is **tracked**, not blocking; they are open items for
follow-up plans.

### 8.1 Compiler-side emit sites for new 800-range codes not yet authored

`IRON_ERR_READONLY_MEMORY = 820` has a stable symbol, the LSP-10
quickfix matches against it, and the fixture-based driver verifies
the handler in isolation. But no `iron_diag_emit(...,
IRON_ERR_READONLY_MEMORY, ...)` call site exists in `src/analyzer/`
yet. The same holds for codes 176, 606, and 613 — the quickfixes are
already wired against the symbols, but the analyzer-side emission
work is deferred to a follow-up plan that lives in `src/analyzer/`
(likely `src/analyzer/check_decl.c` or a new
`src/analyzer/lifecycle.c`).

When that plan lands, the `lsp_publish_diag_820` WILL_FAIL stub in
`tests/lsp/CMakeLists.txt` flips to a real driver verifying the
round-trip through `publishDiagnostics`. The discipline at
`src/diagnostics/diagnostics.h:624` ("emit sites for 800-range
codes MUST live inside iron_compiler") applies.

### 8.2 Additional 800-range codes (810-819, 830-839, 840-899) carry no symbols

The range comment block at `diagnostics.h:96-101` documents the
sub-allocation; the actual `#define` assignments happen on demand as
future memory-model errors need codes. This is deliberate — premature
allocation would invite drift between the documented namespace and
the live symbol table.

### 8.3 The `memory_model_hint.{c,h}` consolidation question remains open

`34-CONTEXT.md` flagged the shared-helper question as Claude's
Discretion. In practice, the string duplication between hover
annotation tokens (`policy: heap`, `regime: unchecked`, ...) and
quickfix titles (`Add 'val'`, `Remove 'readonly'`, ...) turned out
to be modest — the hover tokens are noun-form field labels while
the quickfix titles are imperative verbs. No consolidation was
performed in Phase 34. If LSP follow-up phases push the
duplication count meaningfully higher, the consolidation question
should be revisited.

### 8.4 The "Extract mutating block" variant of LSP-10 emits a placeholder

Per `34-CONTEXT.md` Claude's Discretion, surgical extraction is
out-of-scope for v3.0-alpha.1. The variant edit lays down a TODO
comment + helper-call placeholder rather than performing the
extraction surgery. This is documented behaviour — the variant
title ("Extract mutating block into helper") sets the user's
expectation, and the placeholder gives them a syntactic seed they
finish manually. `ILSP_QUICKFIX_MAX_VARIANTS` stays at 2; no bump
needed.

---

## 9. Architecture notes

### 9.1 The CORE-22 discipline survived the surface push

The structural test `test_core22_single_analyze.c` greps
`src/lsp/` for `iron_analyze_buffer(` (call form, trailing
open-paren — the trailing paren filters out documentation
mentions). The test asserts exactly one hit and asserts that hit
lives in `src/lsp/facade/compile.c`. Every Wave 1-3 plan (hover,
completion, snippet, quickfix) extended the LSP surface without
adding a second analyze call. The hover annotation block consumes
`sym->type` / `decl->declared_type` from the AST. The completion
keyword filter reads only the cursor's source byte context plus the
already-classified `Iron_LspContext`. The defer-free snippet walks
the `Iron_Program` that
`ilsp_facade_compile_for_nav` already returned. The five quickfix
handlers consume the same `Iron_Program` (`ilsp_facade_compile_for_nav`
in some, `ilsp_facade_compile_for_codeaction` in others — both
internal seams route through the single `compile.c` call site).

### 9.2 Hover content is parity-neutral

HARD-24 parity tests (`test_parity_ironc_lsp` / `_fmt` /
`_suggestions`) compare ironc-emitted diagnostic text against
LSP-emitted diagnostic text. The hover annotation block is not part
of the diagnostic surface — it's a render-time embellishment on the
LSP-only hover endpoint. The parity gates therefore did not
constrain hover format choices, but the Phase 34 plans elected to
read every hover field from already-resolved analyzer state so the
implicit-parity invariant ("hover never disagrees with ironc")
holds by construction.

### 9.3 The 800-range emission decoupling

Phase 34 reserved the 800-range namespace and the
`IRON_ERR_READONLY_MEMORY = 820` symbol, but did not author
compiler-side emit sites for the new codes. This is a deliberate
seam: the LSP-10 quickfix handler is verifiable in isolation today
(its synthesized-diagnostic test driver pins the title, range, and
new_text without going through the compiler pipeline), and the
compiler-side emission work lands in a follow-up plan that touches
`src/analyzer/`. Keeping the LSP-side and analyzer-side changes in
separate plans matches the codebase's reviewer-friendly grain.

---

## 10. Cross-phase touchpoints

- **Phases 17-33** attached every annotation Phase 34 surfaces;
  no new AST / types / scope work was required from this phase.
- **Phase 35** picks up the tree-sitter grammar + textmate grammar
  catch-up for the 10 new v3/v4 keywords. They already flow through
  the `build/generated/keyword_mirror.h` drift-guard installed in
  Phase 6 (D-01), so the grammar files regenerate from the same
  source of truth without manual sync.
- **Phase 36** release work consumes this doc as evidence of the
  LSP-side v4 surface readiness alongside `docs/dev/RC-LAYOUT.md`
  and `docs/dev/STDLIB-CONTAINERS.md`.

The four documented residuals (§8.1-8.4) are explicit follow-up
work; they do not block Phase 35 or Phase 36 because none of them
affect the on-disk surface that those phases consume.

---

## 11. Files of record

### Source (added or extended in Phase 34)

- `src/lsp/facade/hover.c` — 4 `derive_*` helpers + `effective_type` + body walker + annotation appender
- `src/lsp/facade/edit/complete/keyword_filter.c` — 10 new strcmp arms + 3 byte-walk helpers
- `src/lsp/facade/edit/complete/snippet.{h,c}` — `ILSP_SNIPPET_DEFER_FREE` + `render_defer_free`
- `src/lsp/facade/edit/complete/defer_free.{h,c}` — backward-scan module (new)
- `src/lsp/facade/edit/complete/complete.c` — `emit_defer_free_snippets` orchestrator hook
- `src/lsp/facade/edit/codeaction/quickfix_missing_val_var.c` — LSP-06 (new)
- `src/lsp/facade/edit/codeaction/quickfix_amp_rc.c` — LSP-09 (new)
- `src/lsp/facade/edit/codeaction/quickfix_forgotten_free.c` — LSP-07 (new)
- `src/lsp/facade/edit/codeaction/quickfix_unused_var_alloc.c` — LSP-08 (new)
- `src/lsp/facade/edit/codeaction/quickfix_readonly_memory.c` — LSP-10 (new)
- `src/lsp/facade/edit/codeaction/registry.{h,c}` — 5 new ASC-sorted entries
- `src/diagnostics/diagnostics.h` — `IRON_ERR_RANGES` block + 800-range + `IRON_ERR_READONLY_MEMORY=820`

### Tests (added in Phase 34)

- `tests/lsp/hover/test_hover_memory_model.c` + 7 hover_*.iron / .expected pairs
- `tests/lsp/complete/test_complete_v4_keywords.c` + 10 complete_keyword_*.iron fixtures
- `tests/lsp/complete/test_complete_defer_free_snippet.c` + 3 complete_defer_free_*.iron fixtures
- `tests/lsp/quickfix/test_quickfix_lsp_{06,07,08,09,10}_*.c` + 5 quickfix_*.iron / .expected_edit pairs + shared `quickfix_fixture_runner.h`
- `tests/lsp/invariant/test_core22_single_analyze.c` — structural CORE-22 grep gate

### Documentation

- `docs/dev/LSP-MEMORY-MODEL.md` — this file
- `.planning/phases/34-lsp-adaptation/34-CLOSEOUT.md` — phase-acceptance summary (local-only, `.planning/` is gitignored)
- `.planning/phases/34-lsp-adaptation/34-{01..05}-SUMMARY.md` — per-plan summaries (local-only)

---

## 12. Verification recipe (silvaserver)

```bash
rsync -az --delete \
  --exclude='build*/' \
  --exclude='.planning/' \
  --exclude='.git/' \
  --exclude='.claude/' \
  ./ 192.168.0.100:/home/victor/iron-lsp-remote/

ssh 192.168.0.100 'cd /home/victor/iron-lsp-remote && podman run --rm \
  --memory=8g --memory-swap=8g \
  -v $(pwd):/work -w /work \
  iron-lsp-build:latest \
  bash -c "
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j\$(nproc) -- -k 0
ctest --test-dir build -L phase34-invariant --output-on-failure
ctest --test-dir build -R test_core22_single_analyze --output-on-failure
ctest --test-dir build -R test_parity_ironc_lsp --output-on-failure
ctest --test-dir build -R test_parity_v3_print_fixed_point --output-on-failure
ctest --test-dir build -R test_lexer_doc_comment_parity --output-on-failure
grep -rn iron_analyze_buffer\\( src/lsp/
grep -rn iron_diag_emit src/lsp/facade/edit/codeaction/ || echo OK_no_violations
"'
```

Expected outcome: `phase34-invariant` 10/10 PASS; parity 5/5 PASS;
CORE-22 GREEN; grep returns exactly one line
`src/lsp/facade/compile.c:57:    Iron_AnalyzeResult r = iron_analyze_buffer(`;
consumer-only grep emits `OK_no_violations`.
