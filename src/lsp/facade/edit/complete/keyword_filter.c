/* Phase 12 Plan 12-02 (KW-03, D-04..D-10) — per-keyword visibility
 * predicate implementation.
 *
 * Pure-read implementation: no allocation, no I/O, no globals. Six v3
 * keyword arms (`pub`, `init`, `readonly`, `pure`, `mut`) plus a default
 * arm matching Phase 4 EDIT-06 behaviour bit-exactly for the 38 pre-v3
 * keywords (D-10).
 *
 * Linear-scan path for `pub` and `init`: iterate `program->decls[]`
 * filtered to IRON_NODE_OBJECT_DECL whose 1-indexed span line range
 * covers the (1-indexed) cursor line. First match wins because object
 * decls cannot nest in Iron's grammar. Cursor inside a hoisted method
 * body still satisfies the OBJECT_DECL span containment test (parser
 * preserves the source object's `span.end_line` past method hoisting,
 * verified at parser.c:3540-3560).
 *
 * Forward/backward byte scans for `readonly`/`pure`/`mut` operate on
 * `doc->text` directly and tolerate broken syntax (Pitfall 10). All
 * scans are bounded by `doc->text_len` (T-12-02-01 mitigation).
 */

#include "lsp/facade/edit/complete/keyword_filter.h"

#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "parser/ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── Static helpers ───────────────────────────────────────────────── */

static bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_';
}

static bool is_ws_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Compute byte offset for (line, col) where both are 0-indexed. Clamps
 * to doc->text_len. */
static size_t cursor_byte_of(const IronLsp_Document *doc,
                              uint32_t line, uint32_t col) {
    if (!doc || !doc->text) return 0;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, line);
    if (line_start > doc->text_len) line_start = doc->text_len;
    size_t cur = line_start + (size_t)col;
    if (cur > doc->text_len) cur = doc->text_len;
    return cur;
}

/* Forward scan from cursor: skip whitespace, then check the next 4 bytes
 * are literal `func` followed by a non-ident boundary. Returns true on
 * exact match. T-12-02-01 mitigation: every memcmp guarded by length
 * check before access. */
static bool kw_modifier_func_follows(const IronLsp_Document *doc,
                                       size_t cursor_byte) {
    if (!doc || !doc->text) return false;
    size_t i = cursor_byte;
    while (i < doc->text_len && is_ws_char(doc->text[i])) i++;
    if (i + 4 > doc->text_len) return false;
    if (memcmp(doc->text + i, "func", 4) != 0) return false;
    /* Boundary: end-of-buffer counts as a boundary. */
    if (i + 4 == doc->text_len) return true;
    return !is_ident_char(doc->text[i + 4]);
}

/* Backward scan: skip ident chars (partial typing), then ws/commas, then
 * require `(` directly before. Then skip whitespace before that and
 * require literal `func` (4 bytes) preceded by a non-ident boundary.
 * T-12-02-02 mitigation: all loops bounded by cursor_byte position
 * (decreasing index). */
static bool kw_mut_in_receiver_pos(const IronLsp_Document *doc,
                                     size_t cursor_byte) {
    if (!doc || !doc->text) return false;
    size_t i = cursor_byte;
    /* Step 1: skip the user's partial ident (the typed-so-far for `mut`). */
    while (i > 0 && is_ident_char(doc->text[i - 1])) i--;
    /* Step 2: skip whitespace + commas (allows `func (a: T, mut|`). */
    while (i > 0) {
        char c = doc->text[i - 1];
        if (is_ws_char(c) || c == ',') { i--; continue; }
        break;
    }
    /* Step 3: previous non-ws/comma must be `(`. */
    if (i == 0 || doc->text[i - 1] != '(') return false;
    i--;
    /* Step 4: skip whitespace. */
    while (i > 0 && is_ws_char(doc->text[i - 1])) i--;
    /* Step 5: require previous 4 bytes = `func`, with non-ident boundary
     * before it (or BOF). */
    if (i < 4) return false;
    if (memcmp(doc->text + i - 4, "func", 4) != 0) return false;
    if (i - 4 == 0) return true;
    return !is_ident_char(doc->text[i - 5]);
}

/* Phase 34 LSP-03 — rhs-of-assign-on-line helper.
 *
 * True when the cursor's line contains a single `=` (not `==`, `!=`, `<=`,
 * `>=`, `:=`) at some column earlier than the cursor. Used by `heap`,
 * `rc`, `weak` visibility arms: these keywords appear in val/var initializer
 * RHS positions (`val x = heap T(...)`, `val w = weak rc null`). The check
 * is byte-walk-only — no analyzer, no tokenizer — so it tolerates broken
 * syntax (mid-typing). T-12-02-01 mitigation: all loops bounded by line
 * range; no read past doc->text_len. */
static bool kw_rhs_of_assign_on_line(const IronLsp_Document *doc,
                                       uint32_t cursor_line_0,
                                       size_t cursor_byte) {
    if (!doc || !doc->text) return false;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, cursor_line_0);
    if (line_start > doc->text_len) line_start = doc->text_len;
    if (cursor_byte > doc->text_len) cursor_byte = doc->text_len;
    if (line_start > cursor_byte) return false;
    /* Walk forward from line_start to cursor_byte; look for a single `=`
     * not adjacent to another comparison operator. */
    for (size_t i = line_start; i < cursor_byte; i++) {
        char c = doc->text[i];
        if (c != '=') continue;
        /* Skip == */
        if (i + 1 < cursor_byte && doc->text[i + 1] == '=') { i++; continue; }
        /* Skip prefixed != <= >= := */
        char prev = (i > line_start) ? doc->text[i - 1] : ' ';
        if (prev == '!' || prev == '<' || prev == '>' || prev == ':') continue;
        return true;
    }
    return false;
}

/* Phase 34 LSP-03 — `*`-precedes helper for the `unchecked` arm.
 *
 * True when (after skipping the cursor's partial identifier backward)
 * the immediately-preceding non-whitespace byte on the same line is `*`.
 * Catches `*unchecked` type annotations where the cursor sits inside
 * the `unchecked` token mid-typing. */
static bool kw_star_precedes_on_line(const IronLsp_Document *doc,
                                       uint32_t cursor_line_0,
                                       size_t cursor_byte) {
    if (!doc || !doc->text) return false;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, cursor_line_0);
    if (line_start > doc->text_len) line_start = doc->text_len;
    if (cursor_byte > doc->text_len) cursor_byte = doc->text_len;
    if (line_start > cursor_byte) return false;
    size_t i = cursor_byte;
    /* Skip the partial identifier (e.g. user has typed `unche`). */
    while (i > line_start && is_ident_char(doc->text[i - 1])) i--;
    /* Skip whitespace. */
    while (i > line_start && (doc->text[i - 1] == ' ' || doc->text[i - 1] == '\t')) i--;
    if (i == line_start) return false;
    return doc->text[i - 1] == '*';
}

/* Phase 34 LSP-03 — `for`-precedes helper for the `in` arm.
 *
 * True when scanning backward from cursor on the same line, after the
 * cursor's partial ident + the loop-variable identifier + intervening
 * whitespace, we hit a literal `for` keyword. Matches the v3
 * `for <var> in <iter>` shape verbatim. */
static bool kw_for_precedes_on_line(const IronLsp_Document *doc,
                                      uint32_t cursor_line_0,
                                      size_t cursor_byte) {
    if (!doc || !doc->text) return false;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, cursor_line_0);
    if (line_start > doc->text_len) line_start = doc->text_len;
    if (cursor_byte > doc->text_len) cursor_byte = doc->text_len;
    if (line_start > cursor_byte) return false;
    size_t i = cursor_byte;
    /* Step 1: skip cursor's partial identifier (typed-so-far for `in`). */
    while (i > line_start && is_ident_char(doc->text[i - 1])) i--;
    /* Step 2: skip whitespace. */
    while (i > line_start && (doc->text[i - 1] == ' ' || doc->text[i - 1] == '\t')) i--;
    /* Step 3: skip a single identifier (the loop variable). */
    while (i > line_start && is_ident_char(doc->text[i - 1])) i--;
    /* Step 4: skip whitespace. */
    while (i > line_start && (doc->text[i - 1] == ' ' || doc->text[i - 1] == '\t')) i--;
    /* Step 5: previous 3 bytes must be `for` with non-ident boundary. */
    if (i < line_start + 3) return false;
    if (memcmp(doc->text + i - 3, "for", 3) != 0) return false;
    if (i - 3 == line_start) return true;
    return !is_ident_char(doc->text[i - 4]);
}

/* Decl-head text-only check: every byte from line-start to cursor_byte
 * is whitespace OR ident-char. Operators/parens/semicolons/dots
 * disqualify. Bounded by line length. */
static bool kw_pub_at_decl_head_textonly(const IronLsp_Document *doc,
                                           uint32_t cursor_line_0,
                                           size_t cursor_byte) {
    if (!doc || !doc->text) return false;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, cursor_line_0);
    if (line_start > doc->text_len) line_start = doc->text_len;
    if (cursor_byte > doc->text_len) cursor_byte = doc->text_len;
    if (line_start > cursor_byte) return false;
    for (size_t i = line_start; i < cursor_byte; i++) {
        char c = doc->text[i];
        if (is_ws_char(c)) continue;
        if (is_ident_char(c)) continue;
        return false;
    }
    return true;
}

/* Linear scan over program->decls[] filtered to IRON_NODE_OBJECT_DECL
 * whose 1-indexed span covers (cursor_line_1). NULL program -> NULL
 * (caller decides lenient/strict fallback per arm). First match wins;
 * Iron's grammar forbids nested object declarations.
 *
 * NB: Iron_Node has NO parent pointer (verified src/parser/ast.h:90-97);
 * this linear scan is the only correct path. Pattern reference:
 * src/lsp/facade/nav/patch_lookup.c:36-52 (`patch_enclosing_in_program`).
 */
static const Iron_Node *enclosing_object_decl(const Iron_Program *program,
                                                 uint32_t cursor_line_1) {
    if (!program) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        const Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        const Iron_Span *sp = &d->span;
        if (cursor_line_1 < sp->line) continue;
        if (cursor_line_1 > sp->end_line) continue;
        return d;
    }
    return NULL;
}

/* ── Public predicate ─────────────────────────────────────────────── */

bool ilsp_keyword_visible_at(const char               *kw,
                              const IronLsp_Document   *doc,
                              const Iron_Program       *program,
                              uint32_t                  cursor_line,
                              uint32_t                  cursor_col,
                              IronLsp_CompletionContext ctx) {
    if (!kw) return false;

    /* All 6 v3 keyword arms need `doc` for byte-buffer scans / line
     * derivation. When `doc == NULL` (test-mode callers passing only a
     * bare Iron_Program) we refuse v3 keywords and fall through to the
     * default arm — preserves Phase 4 EDIT-06 behaviour for the 38
     * pre-v3 keywords. */
    if (doc) {
        size_t cur_byte = cursor_byte_of(doc, cursor_line, cursor_col);
        /* 1-indexed line for span comparisons (Iron_Span uses 1-indexed). */
        uint32_t cursor_line_1 = cursor_line + 1;

        /* 6 v3 keyword arms — order doesn't matter; first match wins. */
        if (strcmp(kw, "pub") == 0) {
            if (!kw_pub_at_decl_head_textonly(doc, cursor_line, cur_byte)) {
                return false;
            }
            const Iron_Node *enc = enclosing_object_decl(program, cursor_line_1);
            /* Visible at module top-level (enc == NULL fallback —
             * covers both "no program yet (broken syntax — be lenient)"
             * and "cursor outside any object decl") OR inside an
             * IRON_NODE_OBJECT_DECL (classic or patch; no is_patch
             * filter). */
            if (enc == NULL) return true;  /* module top-level / lenient */
            return enc->kind == IRON_NODE_OBJECT_DECL;
        }
        if (strcmp(kw, "init") == 0) {
            const Iron_Node *enc = enclosing_object_decl(program, cursor_line_1);
            if (!enc) return false;  /* strict */
            return enc->kind == IRON_NODE_OBJECT_DECL;  /* Pitfall 3 */
        }
        if (strcmp(kw, "readonly") == 0 || strcmp(kw, "pure") == 0) {
            return kw_modifier_func_follows(doc, cur_byte);
        }
        if (strcmp(kw, "mut") == 0) {
            return kw_mut_in_receiver_pos(doc, cur_byte);
        }

        /* ── Phase 34 LSP-03: v3/v4 memory-model keyword visibility. ── */

        /* `heap`, `rc`, `weak`: visible at type-expression positions OR
         * RHS-of-`=` on the current line. Hidden at member/import/
         * statement-head pure-expression positions. */
        if (strcmp(kw, "heap") == 0 ||
            strcmp(kw, "rc")   == 0 ||
            strcmp(kw, "weak") == 0) {
            if (ctx == ILSP_CCTX_MEMBER_AFTER_DOT ||
                ctx == ILSP_CCTX_IMPORT_PATH ||
                ctx == ILSP_CCTX_QUALIFIED_AFTER_COLON) {
                return false;
            }
            if (ctx == ILSP_CCTX_TYPE_POSITION) return true;
            return kw_rhs_of_assign_on_line(doc, cursor_line, cur_byte);
        }

        /* `unchecked`: visible at type-expression positions OR after `*`
         * on the same line (`*unchecked T` annotation form). */
        if (strcmp(kw, "unchecked") == 0) {
            if (ctx == ILSP_CCTX_MEMBER_AFTER_DOT ||
                ctx == ILSP_CCTX_IMPORT_PATH ||
                ctx == ILSP_CCTX_QUALIFIED_AFTER_COLON) {
                return false;
            }
            if (ctx == ILSP_CCTX_TYPE_POSITION) return true;
            if (kw_star_precedes_on_line(doc, cursor_line, cur_byte)) return true;
            return kw_rhs_of_assign_on_line(doc, cursor_line, cur_byte);
        }

        /* `defer`: visible only at statement-head positions. */
        if (strcmp(kw, "defer") == 0) {
            return ctx == ILSP_CCTX_STATEMENT_HEAD;
        }

        /* `drop`, `copy`: visible only inside an Iron_ObjectDecl body
         * (classic or patch) at decl-head text-position. Hidden in
         * function bodies and module top-level. */
        if (strcmp(kw, "drop") == 0 ||
            strcmp(kw, "copy") == 0) {
            const Iron_Node *enc = enclosing_object_decl(program, cursor_line_1);
            if (!enc) return false;
            return kw_pub_at_decl_head_textonly(doc, cursor_line, cur_byte);
        }

        /* `nocopy`: visible at module-decl-head positions (before an
         * `object` keyword: `nocopy object FileHandle { ... }`). Hidden
         * in expression / type-annotation positions. We approximate
         * decl-head with STATEMENT_HEAD ctx plus the decl-head
         * text-only line predicate so `nocopy ` followed by `object`
         * is the canonical visible site. */
        if (strcmp(kw, "nocopy") == 0) {
            if (ctx == ILSP_CCTX_MEMBER_AFTER_DOT ||
                ctx == ILSP_CCTX_IMPORT_PATH ||
                ctx == ILSP_CCTX_QUALIFIED_AFTER_COLON ||
                ctx == ILSP_CCTX_TYPE_POSITION) {
                return false;
            }
            if (ctx != ILSP_CCTX_STATEMENT_HEAD) return false;
            return kw_pub_at_decl_head_textonly(doc, cursor_line, cur_byte);
        }

        /* `leak`: visible at expression / statement-head positions; hidden
         * in type-annotation, member, qualified, and import contexts. */
        if (strcmp(kw, "leak") == 0) {
            return (ctx == ILSP_CCTX_EXPR_HEAD ||
                    ctx == ILSP_CCTX_STATEMENT_HEAD);
        }

        /* `in`: visible only when a `for` token precedes on the same line
         * (the `for <var> in <iter>` shape). Hidden everywhere else. */
        if (strcmp(kw, "in") == 0) {
            return kw_for_precedes_on_line(doc, cursor_line, cur_byte);
        }
    } else {
        /* doc == NULL: refuse the 5 v3 arms that need a byte buffer or
         * staged program. The sixth v3 keyword (`patch`) is context-free
         * at decl-head and falls through to the default arm. Phase 34
         * additions also refuse without doc (all 10 arms need byte-buffer
         * + program access). */
        if (strcmp(kw, "pub")      == 0) return false;
        if (strcmp(kw, "init")     == 0) return false;
        if (strcmp(kw, "readonly") == 0) return false;
        if (strcmp(kw, "pure")     == 0) return false;
        if (strcmp(kw, "mut")      == 0) return false;
        if (strcmp(kw, "heap")      == 0) return false;
        if (strcmp(kw, "rc")        == 0) return false;
        if (strcmp(kw, "weak")      == 0) return false;
        if (strcmp(kw, "unchecked") == 0) return false;
        if (strcmp(kw, "defer")     == 0) return false;
        if (strcmp(kw, "drop")      == 0) return false;
        if (strcmp(kw, "copy")      == 0) return false;
        if (strcmp(kw, "nocopy")    == 0) return false;
        if (strcmp(kw, "leak")      == 0) return false;
        if (strcmp(kw, "in")        == 0) return false;
    }

    /* Default for the 38 pre-v3 keywords (D-10 — preserves Phase 4
     * EDIT-06). */
    (void)cursor_line; (void)cursor_col;
    return (ctx == ILSP_CCTX_EXPR_HEAD || ctx == ILSP_CCTX_STATEMENT_HEAD);
}
