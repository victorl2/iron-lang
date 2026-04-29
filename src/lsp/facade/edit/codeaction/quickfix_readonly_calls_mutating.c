/* Phase 12 Plan 12-03 (QF-05) — quickfix for IRON_ERR_READONLY_CALLS_MUTATING (239).
 *
 * Recipe (D-32, D-33, D-34):
 *   Action A (always): "Drop 'readonly' from caller"
 *     range  = readonly-token span in CALLER method's signature
 *     newText = "" (deletion; absorbs trailing space)
 *   Action B (gated):  "Mark callee as 'readonly'"
 *     range  = zero-width insertion before the CALLEE's `func` token
 *     newText = "readonly "
 *     - Emits only when ALL of the following hold:
 *         (1) wi != NULL,
 *         (2) callee resolved in ares.program->decls[] by method_name,
 *         (3) callee->span.filename == diag->span.filename (same-file
 *             v1 restriction; cross-file deferred per DEF-12-11),
 *         (4) !ilsp_nav_path_is_stdlib(callee->span.filename) (D-34),
 *         (5) find_func_token_for_insert succeeds (callee not already
 *             marked readonly).
 *   Both is_preferred = false.
 *
 * Phase 12 v1 limitation: cross-file Action B requires per-edit URI on
 * IronLsp_CodeAction and is deferred to a later phase (DEF-12-11). Cross-
 * file fixtures exercise the "1 action only" path (Action A; Action B
 * suppressed) — same shape as the stdlib carve-out.
 *
 * Methods are HOISTED to top-level program->decls[] as
 * IRON_NODE_METHOD_DECL nodes per Phase 9 D-06/D-07 — Iron_ObjectDecl
 * has NO methods[] field. Linear scan over program->decls[] filtered to
 * IRON_NODE_METHOD_DECL with strcmp on method_name; declaration order
 * wins (no overload resolution in Iron v3 — verified by absence of
 * mangling in src/analyzer/resolve.c).
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/workspace_index.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Find the enclosing method/func decl whose span line range contains
 * diag->span.line (caller of the mutating method). Filename pointer
 * equality + line range. Linear scan over program->decls[]. */
static const Iron_Node *find_enclosing_method(const Iron_Program  *program,
                                                 const Iron_Diagnostic *diag) {
    if (!program || !diag) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_METHOD_DECL) {
            const Iron_MethodDecl *m = (const Iron_MethodDecl *)d;
            if (m->span.filename != diag->span.filename) continue;
            if (diag->span.line >= m->span.line &&
                diag->span.line <= m->span.end_line) {
                return d;
            }
        } else if (d->kind == IRON_NODE_FUNC_DECL) {
            const Iron_FuncDecl *fn = (const Iron_FuncDecl *)d;
            if (fn->span.filename != diag->span.filename) continue;
            if (diag->span.line >= fn->span.line &&
                diag->span.line <= fn->span.end_line) {
                return d;
            }
        }
    }
    return NULL;
}

/* Lex a slice of doc->text and locate the first IRON_TOK_READONLY token
 * inside the given method/func decl's signature. Same pattern as QF-04;
 * intentional duplication — file-scope + small enough that lifting to a
 * shared helper deferred per CONTEXT.md "Claude's Discretion" (lift
 * threshold = 3rd consumer; QF-04 + QF-05 = 2). */
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
    Iron_Lexer    lex   = iron_lexer_create(slice, "<qf05-ro>", arena, &diags);
    Iron_Token   *toks  = iron_lex_all(&lex);
    if (!toks) {
        iron_diaglist_free(&diags);
        return false;
    }
    bool found = false;
    for (size_t i = 0; toks[i].kind != IRON_TOK_EOF; i++) {
        if (toks[i].kind == IRON_TOK_READONLY) {
            *out_line = start_line0 + toks[i].line;
            *out_col  = toks[i].col;
            *out_len  = toks[i].len;
            found = true;
            break;
        }
    }
    iron_diaglist_free(&diags);
    return found;
}

/* Extract the callee method name from the diag->span line. The diag
 * span starts at the call expression's receiver (`self.bump()` →
 * span starts at `self`). Find the `(` at-or-after diag->span.col,
 * then scan backwards over identifier characters to capture the bare
 * method name. Returns arena-interned name on success, NULL on miss
 * or lex failure.
 *
 * Implementation: re-lex the diag->span line slice and walk tokens
 * forward to find IRON_TOK_LPAREN; the token immediately preceding
 * IRON_TOK_LPAREN is the method name (an IRON_TOK_IDENTIFIER preceded
 * by `.` for receiver-form calls). */
static const char *extract_callee_name_from_span(
    const struct IronLsp_Document *doc,
    Iron_Arena                    *arena,
    const Iron_Diagnostic         *diag) {
    if (!doc || !arena || !diag) return NULL;
    if (diag->span.line == 0) return NULL;
    /* Slice covers the diag's span line(s). */
    uint32_t start_line0 = diag->span.line - 1;
    uint32_t end_line0   = (diag->span.end_line > 0)
                              ? diag->span.end_line - 1
                              : start_line0;
    /* Add a 1-line tolerance for multiline call expressions. */
    end_line0 += 1;
    size_t slice_start = ilsp_byte_of_line(&doc->line_idx, start_line0);
    size_t slice_end   = ilsp_byte_of_line(&doc->line_idx, end_line0 + 1);
    if (slice_end > doc->text_len) slice_end = doc->text_len;
    if (slice_start >= slice_end || slice_start >= doc->text_len) return NULL;
    size_t slice_len = slice_end - slice_start;

    char *slice = (char *)iron_arena_alloc(arena, slice_len + 1, 1);
    if (!slice) return NULL;
    memcpy(slice, doc->text + slice_start, slice_len);
    slice[slice_len] = '\0';

    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer    lex   = iron_lexer_create(slice, "<qf05-callee>", arena, &diags);
    Iron_Token   *toks  = iron_lex_all(&lex);
    if (!toks) {
        iron_diaglist_free(&diags);
        return NULL;
    }
    /* Walk to the first IRON_TOK_LPAREN; the immediately-preceding
     * IRON_TOK_IDENTIFIER is the method name. */
    const char *name = NULL;
    Iron_TokenKind prev_kind = IRON_TOK_COUNT;
    const char    *prev_value = NULL;
    for (size_t i = 0; toks[i].kind != IRON_TOK_EOF; i++) {
        if (toks[i].kind == IRON_TOK_LPAREN &&
            prev_kind == IRON_TOK_IDENTIFIER && prev_value) {
            /* prev_value lives on `arena` (the lexer's arena), so it is
             * already arena-owned. Return it as-is. */
            name = prev_value;
            break;
        }
        prev_kind  = toks[i].kind;
        prev_value = toks[i].value;
    }
    iron_diaglist_free(&diags);
    return name;
}

/* Lex the callee's signature and locate the IRON_TOK_FUNC token byte
 * position. Refuses (returns false) when the `func` token is already
 * preceded by IRON_TOK_READONLY (idempotency: never insert twice).
 * Returns the global 1-indexed (line, col) of the `func` token. */
static bool find_func_token_for_insert(const struct IronLsp_Document *doc,
                                          Iron_Arena                    *arena,
                                          const Iron_MethodDecl         *callee,
                                          uint32_t                      *out_line,
                                          uint32_t                      *out_col) {
    if (!doc || !arena || !callee) return false;
    if (callee->span.line == 0) return false;
    uint32_t start_line0 = callee->span.line - 1;
    uint32_t end_line0;
    if (callee->body && callee->body->span.line > 0) {
        end_line0 = callee->body->span.line - 1;
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
    Iron_Lexer    lex   = iron_lexer_create(slice, "<qf05-func>", arena, &diags);
    Iron_Token   *toks  = iron_lex_all(&lex);
    if (!toks) {
        iron_diaglist_free(&diags);
        return false;
    }
    bool found = false;
    bool prev_is_readonly = false;
    for (size_t i = 0; toks[i].kind != IRON_TOK_EOF; i++) {
        if (toks[i].kind == IRON_TOK_READONLY) {
            prev_is_readonly = true;
            continue;
        }
        if (toks[i].kind == IRON_TOK_FUNC) {
            if (prev_is_readonly) {
                /* Already readonly — refuse Action B. */
                break;
            }
            *out_line = start_line0 + toks[i].line;
            *out_col  = toks[i].col;
            found = true;
            break;
        }
        /* Any other token resets the readonly-streak. */
        prev_is_readonly = false;
    }
    iron_diaglist_free(&diags);
    return found;
}

/* ── Entry point ─────────────────────────────────────────────────── */

void ilsp_quickfix_readonly_calls_mutating(const Iron_Diagnostic           *diag,
                                              struct IronLsp_Document         *doc,
                                              struct IronLsp_WorkspaceIndex   *wi,
                                              Iron_Arena                      *arena,
                                              IronLsp_CodeAction              *out_arr,
                                              size_t                           out_cap,
                                              size_t                          *out_n)
{
    if (!out_arr || !out_n) return;
    *out_n = 0;
    if (out_cap == 0) return;
    memset(&out_arr[0], 0, sizeof(out_arr[0]));
    if (out_cap >= 2) memset(&out_arr[1], 0, sizeof(out_arr[1]));
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

    const Iron_Node *caller = find_enclosing_method(program, diag);
    if (!caller) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    uint32_t ro_line_1 = 0, ro_col_1 = 0, ro_len = 0;
    if (!find_readonly_token(doc, arena, caller,
                                &ro_line_1, &ro_col_1, &ro_len)) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    uint32_t ro_line_0 = ro_line_1 - 1;
    uint32_t ro_col_0  = ro_col_1 - 1;

    /* Action A: always emit — drop readonly from caller. */
    out_arr[0].title             = "Drop 'readonly' from caller";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = false;
    out_arr[0].edit_start_line   = ro_line_0;
    out_arr[0].edit_start_char   = ro_col_0;
    out_arr[0].edit_end_line     = ro_line_0;
    out_arr[0].edit_end_char     = ro_col_0 + ro_len + 1;
    out_arr[0].edit_new_text     = "";
    size_t emitted = 1;

    /* Action B feasibility (D-34 + W-2 same-file v1).
     *
     * AST shape (verified src/parser/ast.h:273-332 + Phase 9 D-06/D-07):
     *   - Iron_ObjectDecl has NO `methods[]` field. The parser HOISTS
     *     methods (regular, init, named-init, patch) to top-level
     *     program->decls[] as standalone IRON_NODE_METHOD_DECL nodes.
     *     Their span is fully nested inside the source object's
     *     OBJECT_DECL span; Iron_MethodDecl.type_name carries the
     *     receiver type, .method_name the method identifier.
     *   - Therefore the callee walk is a linear scan over
     *     ares.program->decls[] filtered to IRON_NODE_METHOD_DECL
     *     (NOT a nested walk into od->methods[] — that array does
     *     not exist). Pattern reference:
     *     src/lsp/facade/nav/patch_lookup.c:36-52.
     *   - Same-file gate uses md->span.filename pointer-equality
     *     against diag->span.filename (Iron_Span.filename is
     *     arena-interned per CLAUDE.md "Architecture →
     *     Iron_Span.filename is an arena-interned pointer"). */
    if (out_cap >= 2 && wi) {
        /* Step 1: Re-tokenize diag->span line to extract the callee
         * method name. */
        const char *callee_name = extract_callee_name_from_span(doc, arena, diag);
        if (!callee_name) goto emit_a_only;

        /* Step 2: Linear scan ares.program->decls[] for
         * IRON_NODE_METHOD_DECL matching callee_name. First match in
         * declaration order wins (no overload resolution in Iron v3). */
        const Iron_MethodDecl *callee = NULL;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
            const Iron_MethodDecl *md = (const Iron_MethodDecl *)d;
            if (!md->method_name) continue;
            if (strcmp(md->method_name, callee_name) == 0) {
                callee = md;
                break;
            }
        }
        if (!callee) goto emit_a_only;

        /* Step 3: Same-file gate (Phase 12 v1 restriction; cross-file
         * deferred per DEF-12-11). Iron_Span.filename is arena-interned;
         * pointer equality is the canonical comparison. */
        if (callee->span.filename != diag->span.filename) goto emit_a_only;

        /* Step 4: Stdlib carve-out (D-34). */
        const char *callee_path = callee->span.filename;
        if (!callee_path || ilsp_nav_path_is_stdlib(callee_path)) goto emit_a_only;

        /* Step 5: Locate the `func` token byte position; refuse if
         * already preceded by `readonly` (idempotency). */
        uint32_t func_line_1 = 0, func_col_1 = 0;
        if (!find_func_token_for_insert(doc, arena, callee,
                                            &func_line_1, &func_col_1)) {
            goto emit_a_only;
        }

        /* Step 6: Emit Action B — insert "readonly " before the func token. */
        uint32_t func_line_0 = func_line_1 - 1;
        uint32_t func_col_0  = func_col_1 - 1;
        out_arr[1].title             = "Mark callee as 'readonly'";
        out_arr[1].kind              = "quickfix";
        out_arr[1].originating_diag  = diag;
        out_arr[1].is_preferred      = false;
        out_arr[1].edit_start_line   = func_line_0;
        out_arr[1].edit_start_char   = func_col_0;
        out_arr[1].edit_end_line     = func_line_0;
        out_arr[1].edit_end_char     = func_col_0;
        out_arr[1].edit_new_text     = "readonly ";
        emitted = 2;
    }
emit_a_only:

    *out_n = emitted;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
