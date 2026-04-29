/* Phase 12 Plan 12-03 (QF-04) — quickfix for IRON_ERR_READONLY_WRITE_SELF (238).
 *
 * Recipe (D-29, D-30, D-31):
 *   2 CodeActions; both is_preferred = false (semantic ambiguity — the
 *   "right" answer depends on whether the user intended the method to
 *   mutate state or not; D-31 forbids picking one).
 *   Action A: title = "Remove 'readonly' modifier"
 *     range  = readonly-token span in caller method's signature
 *     newText = "" (deletion; absorbs the trailing space too)
 *   Action B: title = "Remove offending write"
 *     range  = diag->span (the write expression's full span)
 *     newText = ""
 *   data_variant_idx is set by codeaction.c orchestrator (0 for A, 1 for B).
 *
 * Methods are HOISTED to top-level program->decls[] as
 * IRON_NODE_METHOD_DECL nodes per Phase 9 D-06/D-07; the enclosing-method
 * walker is a linear scan over program->decls[] filtered to method/func
 * decls whose body span contains diag->span. Uses arena-interned filename
 * pointer-equality + line containment.
 *
 * Refusal cases set *out_n = 0 early; partial emission is NOT supported
 * (either both or none — D-31 semantic ambiguity).
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── Helpers shared structurally with QF-05 ───────────────────────── */

/* Same-file gate: returns true when both filenames are NULL OR
 * pointer-equal OR string-equal. Pointer equality is the optimistic
 * fast path (analyzer arena interning); string equality is the
 * fallback for re-analyzed programs that allocate fresh arena strings.
 * NULL on either side is treated as "match anything" (defensive — the
 * orchestrator always supplies non-NULL spans, but the handler must
 * not crash on a malformed diag). */
static bool same_file(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return true;
    return strcmp(a, b) == 0;
}

/* Find the enclosing method/func decl whose body span contains the
 * 1-indexed diag->span.line. Walks program->decls[] linearly:
 *   - IRON_NODE_METHOD_DECL: methods (including init) hoisted to top
 *     level. Match same-file via filename string equality (pointer
 *     equality is unreliable across re-analyze arena boundaries) +
 *     line containment of method's full span.
 *   - IRON_NODE_FUNC_DECL: top-level funcs. Phase 84 already rejects
 *     readonly/pure on top-level funcs (E0245), so a body containing
 *     E0238 is necessarily a method — but we accept either kind for
 *     defensive completeness.
 * Returns the matched node or NULL. */
static const Iron_Node *find_enclosing_method(const Iron_Program  *program,
                                                 const Iron_Diagnostic *diag) {
    if (!program || !diag) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_METHOD_DECL) {
            const Iron_MethodDecl *m = (const Iron_MethodDecl *)d;
            if (!same_file(m->span.filename, diag->span.filename)) continue;
            if (diag->span.line >= m->span.line &&
                diag->span.line <= m->span.end_line) {
                return d;
            }
        } else if (d->kind == IRON_NODE_FUNC_DECL) {
            const Iron_FuncDecl *fn = (const Iron_FuncDecl *)d;
            if (!same_file(fn->span.filename, diag->span.filename)) continue;
            if (diag->span.line >= fn->span.line &&
                diag->span.line <= fn->span.end_line) {
                return d;
            }
        }
    }
    return NULL;
}

/* Lex a slice of doc->text spanning the method/func signature line(s)
 * and locate the first IRON_TOK_READONLY token. Returns its 1-indexed
 * (line, col) in global document coordinates plus its byte length.
 *
 * The signature lives between method->span.line (the `func`/`readonly`
 * line, 1-indexed) and the line BEFORE the body's opening brace. We lex
 * a generous slice (signature line + a couple of lines) bounded by
 * doc->text_len.
 *
 * Returns true on hit; false on lex failure or no `readonly` token.
 * NULL-safe — refusal on every NULL input. */
static bool find_readonly_token(const struct IronLsp_Document *doc,
                                   Iron_Arena *arena,
                                   const Iron_Node *method_decl,
                                   uint32_t *out_line,
                                   uint32_t *out_col,
                                   uint32_t *out_len) {
    if (!doc || !arena || !method_decl) return false;
    Iron_Span sig_span;
    Iron_Node *body = NULL;
    if (method_decl->kind == IRON_NODE_METHOD_DECL) {
        const Iron_MethodDecl *m = (const Iron_MethodDecl *)method_decl;
        sig_span = m->span;
        body     = m->body;
    } else if (method_decl->kind == IRON_NODE_FUNC_DECL) {
        const Iron_FuncDecl *fn = (const Iron_FuncDecl *)method_decl;
        sig_span = fn->span;
        body     = fn->body;
    } else {
        return false;
    }

    /* Slice: from start of signature line through end-of-line of the
     * line containing the body's opening `{`. If body is NULL, fall back
     * to a 4-line tolerance. */
    if (sig_span.line == 0) return false;
    uint32_t start_line0 = sig_span.line - 1;
    uint32_t end_line0;
    if (body && body->span.line > 0) {
        end_line0 = body->span.line - 1;
    } else {
        end_line0 = start_line0 + 4;
    }
    size_t slice_start = ilsp_byte_of_line(&doc->line_idx, start_line0);
    size_t slice_end   = ilsp_byte_of_line(&doc->line_idx, end_line0 + 1);
    if (slice_end > doc->text_len) slice_end = doc->text_len;
    if (slice_start >= slice_end || slice_start >= doc->text_len) return false;
    size_t slice_len = slice_end - slice_start;

    char *slice = (char *)iron_arena_alloc(arena, slice_len + 1, 1);
    if (!slice) return false;
    memcpy(slice, doc->text + slice_start, slice_len);
    slice[slice_len] = '\0';

    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer    lex   = iron_lexer_create(slice, "<qf04-slice>", arena, &diags);
    Iron_Token   *toks  = iron_lex_all(&lex);
    if (!toks) {
        iron_diaglist_free(&diags);
        return false;
    }

    bool found = false;
    for (size_t i = 0; toks[i].kind != IRON_TOK_EOF; i++) {
        if (toks[i].kind == IRON_TOK_READONLY) {
            *out_line = start_line0 + toks[i].line;  /* 1-indexed global */
            *out_col  = toks[i].col;                 /* 1-indexed global */
            *out_len  = toks[i].len;
            found = true;
            break;
        }
    }
    iron_diaglist_free(&diags);
    return found;
}

/* ── Entry point ─────────────────────────────────────────────────── */

void ilsp_quickfix_readonly_write_self(const Iron_Diagnostic           *diag,
                                          struct IronLsp_Document         *doc,
                                          struct IronLsp_WorkspaceIndex   *wi,
                                          Iron_Arena                      *arena,
                                          IronLsp_CodeAction              *out_arr,
                                          size_t                           out_cap,
                                          size_t                          *out_n)
{
    (void)wi;
    if (!out_arr || !out_n) return;
    *out_n = 0;
    if (out_cap < 2) return;
    memset(&out_arr[0], 0, sizeof(out_arr[0]));
    memset(&out_arr[1], 0, sizeof(out_arr[1]));
    if (!diag || !doc || !arena) return;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = NULL };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    const Iron_Node *method = find_enclosing_method(program, diag);
    if (!method) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    uint32_t ro_line_1 = 0, ro_col_1 = 0, ro_len = 0;
    if (!find_readonly_token(doc, arena, method,
                                &ro_line_1, &ro_col_1, &ro_len)) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Convert to LSP 0-indexed coords. The deletion absorbs the trailing
     * space after `readonly` so the resulting `func ...` has no double
     * space. ro_col_1 is 1-indexed; the token's start char (0-indexed)
     * is ro_col_1 - 1; end char (exclusive) is start + ro_len + 1 (the
     * +1 swallows the trailing space). */
    uint32_t ro_line_0 = ro_line_1 - 1;
    uint32_t ro_col_0  = ro_col_1 - 1;

    /* Action A: delete `readonly ` from caller method signature. */
    out_arr[0].title             = "Remove 'readonly' modifier";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = false;
    out_arr[0].edit_start_line   = ro_line_0;
    out_arr[0].edit_start_char   = ro_col_0;
    out_arr[0].edit_end_line     = ro_line_0;
    out_arr[0].edit_end_char     = ro_col_0 + ro_len + 1;  /* +1 for trailing space */
    out_arr[0].edit_new_text     = "";

    /* Action B: delete the offending write expression. */
    IronLsp_Range r = ilsp_span_to_lsp_range(diag->span, doc, ILSP_ENC_UTF8);
    out_arr[1].title             = "Remove offending write";
    out_arr[1].kind              = "quickfix";
    out_arr[1].originating_diag  = diag;
    out_arr[1].is_preferred      = false;
    out_arr[1].edit_start_line   = r.start.line;
    out_arr[1].edit_start_char   = r.start.character;
    out_arr[1].edit_end_line     = r.end.line;
    out_arr[1].edit_end_char     = r.end.character;
    out_arr[1].edit_new_text     = "";

    *out_n = 2;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
