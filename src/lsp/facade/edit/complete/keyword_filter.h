#ifndef IRON_LSP_FACADE_EDIT_COMPLETE_KEYWORD_FILTER_H
#define IRON_LSP_FACADE_EDIT_COMPLETE_KEYWORD_FILTER_H

/* Phase 12 Plan 12-02 (KW-03, D-04..D-10) — per-keyword visibility predicate.
 *
 * Returns true iff `kw` should appear as a completion candidate at the
 * cursor. Falls back to (ctx == EXPR_HEAD || STATEMENT_HEAD) for the 38
 * pre-v3 keywords (D-10 — bit-exactly preserves Phase 4 EDIT-06 behaviour).
 *
 * Per-keyword arms for the 6 v3 keywords (D-04..D-09):
 *   - pub      decl-head only (line-prefix is whitespace/ident; enclosing
 *              scope is module top-level OR an Iron_ObjectDecl body —
 *              classic OR patch).
 *   - init     enclosing scope is an Iron_ObjectDecl body (classic OR
 *              patch — Pitfall 3); strict refusal otherwise.
 *   - readonly forward byte-buffer scan: literal `func` follows after
 *              optional whitespace.
 *   - pure     same as readonly.
 *   - mut      receiver-parameter position: backward scan past partial
 *              ident + ws/commas hits `(`, and the previous 4 bytes
 *              (skipping ws) are literal `func`.
 *
 * Pure read: no allocation, no I/O, no globals. Safe under concurrent
 * request handling per CLAUDE.md "Concurrency: requests may run in parallel
 * on shared iron_compiler state — all state must be scoped per
 * request/document".
 *
 * Linear-scan path (verified W-3 fix): `pub` and `init` consult a small
 * file-private helper that iterates the staged Iron_Program's `decls[]`
 * filtered to IRON_NODE_OBJECT_DECL. Iron_Node has no parent pointer
 * (src/parser/ast.h:90-97), so the parent-walk path is structurally
 * impossible. The hoisted-method case (Phase 9 D-06/D-07) is handled by
 * the OBJECT_DECL span containment test — methods nested inside an object
 * body satisfy `cursor_line in [span.line .. span.end_line]`. */

#include "lsp/facade/edit/complete/context_classify.h"
#include "parser/ast.h"  /* Iron_Program — typedef over anonymous struct */

#include <stdbool.h>
#include <stdint.h>

struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

/* `kw` and `doc` are required; NULL inputs return false. cursor_line and
 * cursor_col are 0-indexed (LSP convention). ctx is consumed by the
 * default arm to mirror Phase 4 EDIT-06 behaviour for the 38 pre-v3
 * keywords.
 *
 * `program` is the staged Iron_Program for `doc`. When the document has
 * not been analyzed yet OR analyze failed (parse-fatal / cold-start),
 * pass NULL; the `pub` arm degrades gracefully (lenient: byte-scan only)
 * and the `init` arm refuses (strict: avoid noise inside expression
 * bodies that happen to be parseable). */
bool ilsp_keyword_visible_at(const char                       *kw,
                              const struct IronLsp_Document    *doc,
                              const Iron_Program               *program,
                              uint32_t                          cursor_line,
                              uint32_t                          cursor_col,
                              IronLsp_CompletionContext         ctx);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_EDIT_COMPLETE_KEYWORD_FILTER_H */
