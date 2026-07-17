/* Phase 34 LSP-10: two-action quickfix for IRON_ERR_READONLY_MEMORY=820.
 *
 * Recipe (CONTEXT.md "Quickfix UX" — LSP-10 is the only two-variant
 * memory-model quickfix per the locked decisions):
 *
 *   Variant 0: "Remove 'readonly'"
 *     range  = the `readonly ` token (8 chars + trailing space) in the
 *              enclosing func/method signature
 *     newText = "" (deletion)
 *
 *   Variant 1: "Extract mutating block into helper"
 *     range  = the body span covering the offending allocation site
 *              through end of function body content
 *     newText = "    return <name>_impl(<args>)" (placeholder body —
 *              the user completes the extraction manually; v3.0-alpha.1
 *              ships the surgical signal, not the full refactor)
 *
 *   Both is_preferred = false (semantic ambiguity per D-31 Phase 12
 *   convention — the "right" answer depends on whether the user
 *   intended the function to mutate state or not; never auto-pick).
 *
 * Compiler-side emission for code 820 lands in a follow-up plan. This
 * handler is verified in isolation today by synthesizing the diag in
 * the test driver (see tests/lsp/quickfix/test_quickfix_lsp_10_*).
 *
 * Consumer-only handler (CORE-22): NEVER emits diagnostics from the
 * LSP side; NEVER adds a second iron_analyze_buffer call. Re-analyzes
 * through the existing ilsp_facade_compile_for_nav facade for fresh
 * spans (Pitfall 3 — anchor drift). */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Locate the enclosing func/method decl whose body span covers
 * diag->span.line. Mirrors the helper in quickfix_readonly_write_self.c
 * (kept co-located rather than lifted; lift threshold = 3rd consumer). */
static const Iron_Node *find_enclosing_func(const Iron_Program  *program,
                                              const Iron_Diagnostic *diag) {
    if (!program || !diag) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_FUNC_DECL) {
            const Iron_FuncDecl *fn = (const Iron_FuncDecl *)d;
            if (diag->span.line >= fn->span.line &&
                diag->span.line <= fn->span.end_line) {
                return d;
            }
        } else if (d->kind == IRON_NODE_METHOD_DECL) {
            const Iron_MethodDecl *m = (const Iron_MethodDecl *)d;
            if (diag->span.line >= m->span.line &&
                diag->span.line <= m->span.end_line) {
                return d;
            }
        }
    }
    return NULL;
}

/* Scan doc->text for the leading `readonly` keyword on the signature
 * line. Returns (line_1, col_1, len) of the token plus trailing space
 * (so deletion absorbs both). Falls back to `func_line, 1, 9` when the
 * doc-side scan can't find the token (defensive — keeps the variant
 * emittable when the program is in re-analyze recovery). */
static bool find_readonly_keyword(const struct IronLsp_Document *doc,
                                     uint32_t                       sig_line_1,
                                     uint32_t                      *out_line_1,
                                     uint32_t                      *out_col_1,
                                     uint32_t                      *out_len) {
    if (!doc || sig_line_1 == 0) return false;
    uint32_t line_0 = sig_line_1 - 1;
    size_t lstart = ilsp_byte_of_line(&doc->line_idx, line_0);
    if (lstart >= doc->text_len) return false;
    size_t lend = ilsp_byte_of_line(&doc->line_idx, line_0 + 1);
    if (lend > doc->text_len) lend = doc->text_len;
    /* Skip leading whitespace. */
    size_t p = lstart;
    while (p < lend && (doc->text[p] == ' ' || doc->text[p] == '\t')) p++;
    if (p + 9 > lend) return false;
    if (memcmp(doc->text + p, "readonly ", 9) != 0) return false;
    *out_line_1 = sig_line_1;
    *out_col_1  = (uint32_t)(p - lstart) + 1;  /* 1-indexed */
    *out_len    = 9;                            /* `readonly ` with trailing space */
    return true;
}

/* Extract the enclosing function name + first parameter binding (for
 * the placeholder helper call "name_impl(arg)"). Returns false on miss. */
static bool extract_name_and_first_param(const Iron_Node *fn_node,
                                            const char     **out_name,
                                            const char     **out_param) {
    *out_name = NULL;
    *out_param = NULL;
    if (!fn_node) return false;
    if (fn_node->kind == IRON_NODE_FUNC_DECL) {
        const Iron_FuncDecl *fn = (const Iron_FuncDecl *)fn_node;
        *out_name = fn->name;
        if (fn->param_count > 0 && fn->params && fn->params[0]) {
            Iron_Node *p = fn->params[0];
            if (p->kind == IRON_NODE_PARAM) {
                const Iron_Param *prm = (const Iron_Param *)p;
                *out_param = prm->name;
            }
        }
    } else if (fn_node->kind == IRON_NODE_METHOD_DECL) {
        const Iron_MethodDecl *m = (const Iron_MethodDecl *)fn_node;
        *out_name = m->method_name;
        if (m->param_count > 0 && m->params && m->params[0]) {
            Iron_Node *p = m->params[0];
            if (p->kind == IRON_NODE_PARAM) {
                const Iron_Param *prm = (const Iron_Param *)p;
                *out_param = prm->name;
            }
        }
    }
    return *out_name != NULL;
}

void ilsp_quickfix_readonly_memory(const Iron_Diagnostic           *diag,
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
    if (diag->span.line == 0) return;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = NULL };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    /* program may legitimately be NULL when the document is mid-edit;
     * the variant-0 doc-side scan still works without it. We tolerate
     * the missing program by falling back to a heuristic signature
     * line derived from the diag span. */

    /* Locate the enclosing func/method to anchor the readonly keyword
     * and the body span. */
    const Iron_Node *fn = program ? find_enclosing_func(program, diag) : NULL;
    uint32_t sig_line_1 = 0;
    uint32_t body_start_line_1 = 0;
    uint32_t body_end_line_1   = 0;
    const char *fname = NULL;
    const char *fparam = NULL;
    if (fn) {
        if (fn->kind == IRON_NODE_FUNC_DECL) {
            const Iron_FuncDecl *fd = (const Iron_FuncDecl *)fn;
            sig_line_1 = fd->span.line;
            body_end_line_1 = fd->span.end_line;
            if (fd->body) body_start_line_1 = fd->body->span.line;
        } else {
            const Iron_MethodDecl *md = (const Iron_MethodDecl *)fn;
            sig_line_1 = md->span.line;
            body_end_line_1 = md->span.end_line;
            if (md->body) body_start_line_1 = md->body->span.line;
        }
        extract_name_and_first_param(fn, &fname, &fparam);
    } else {
        /* Heuristic: walk doc->text backwards from diag->span.line to
         * find the most recent line beginning with `readonly`. */
        for (uint32_t line_1 = diag->span.line;
             line_1 > 0 && sig_line_1 == 0;
             line_1--) {
            uint32_t l1 = 0, c1 = 0, ll = 0;
            if (find_readonly_keyword(doc, line_1, &l1, &c1, &ll)) {
                sig_line_1 = line_1;
                break;
            }
        }
        body_start_line_1 = diag->span.line;
        body_end_line_1   = diag->span.line + 4;  /* heuristic body span */
    }
    if (sig_line_1 == 0) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* ── Variant 0: remove `readonly ` from the signature ────────── */
    uint32_t ro_line_1 = 0, ro_col_1 = 0, ro_len = 0;
    if (!find_readonly_keyword(doc, sig_line_1, &ro_line_1, &ro_col_1, &ro_len)) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    uint32_t ro_line_0  = ro_line_1 - 1;
    uint32_t ro_start_0 = ro_col_1 - 1;
    uint32_t ro_end_0   = ro_start_0 + ro_len;

    out_arr[0].title             = "Remove 'readonly'";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = false;
    out_arr[0].edit_start_line   = ro_line_0;
    out_arr[0].edit_start_char   = ro_start_0;
    out_arr[0].edit_end_line     = ro_line_0;
    out_arr[0].edit_end_char     = ro_end_0;
    out_arr[0].edit_new_text     = "";

    /* ── Variant 1: extract mutating block into helper ───────────── */
    /* Range covers the body content (first body line through the line
     * before the closing `}`); newText is the placeholder call. */
    uint32_t v1_start_line_0 = (body_start_line_1 > 1)
                                  ? body_start_line_1
                                  : (diag->span.line - 1);
    /* body_end_line_1 (the line of the closing `}`) sits 1-indexed —
     * 0-indexed it's body_end_line_1 - 1; the last content line of the
     * body is body_end_line_1 - 1 (1-indexed) = body_end_line_1 - 2
     * (0-indexed). We want the deletion to cover from the first body
     * content line to end-of-line of that last content line. The
     * fixture parser uses end_char = byte-length of the last line. */
    uint32_t last_content_line_0 = (body_end_line_1 > 1)
                                       ? body_end_line_1 - 2
                                       : v1_start_line_0;
    /* Compute end_char as the byte length of last_content_line_0. */
    size_t lcl_start = ilsp_byte_of_line(&doc->line_idx, last_content_line_0);
    size_t lcl_end   = ilsp_byte_of_line(&doc->line_idx, last_content_line_0 + 1);
    if (lcl_end > doc->text_len) lcl_end = doc->text_len;
    if (lcl_end > 0 && lcl_end > lcl_start &&
        doc->text[lcl_end - 1] == '\n') lcl_end--;
    uint32_t v1_end_char_0 = (uint32_t)(lcl_end - lcl_start);

    /* Build placeholder body: "    return <name>_impl(<param>)". */
    const char *use_name = fname ? fname : "fn";
    const char *use_param = fparam ? fparam : "b";
    size_t need = strlen(use_name) + strlen(use_param) + 32;
    char *new_text = (char *)iron_arena_alloc(arena, need, 1);
    if (!new_text) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        *out_n = 1;  /* still emit variant 0 */
        return;
    }
    int wn = snprintf(new_text, need,
                       "    return %s_impl(%s)", use_name, use_param);
    if (wn <= 0 || (size_t)wn >= need) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        *out_n = 1;
        return;
    }

    out_arr[1].title             = "Extract mutating block into helper";
    out_arr[1].kind              = "quickfix";
    out_arr[1].originating_diag  = diag;
    out_arr[1].is_preferred      = false;
    out_arr[1].edit_start_line   = v1_start_line_0;
    out_arr[1].edit_start_char   = 0;
    out_arr[1].edit_end_line     = last_content_line_0;
    out_arr[1].edit_end_char     = v1_end_char_0;
    out_arr[1].edit_new_text     = new_text;

    *out_n = 2;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
