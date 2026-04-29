/* Phase 12 Plan 12-03 (QF-03) — quickfix for IRON_ERR_V3_INLINE_DEFAULT (262).
 *
 * Recipe (D-26, D-27, D-28):
 *   title         = "Move default into init(...)"
 *   kind          = "quickfix"
 *   is_preferred  = true (mechanical fix; user clearly intended init-time)
 *   edit_text_edits[2] = {
 *     [0] = { range: <field's "= expr" span>, newText: "" },          // delete inline default
 *     [1] = { range: <init body insertion>,    newText: "<assign>" }   // append/synthesize
 *   }
 *
 * Two branches:
 *   - Existing init body: append `<indent>self.<f> = <expr>\n` before
 *     the init's closing `}` (insertion at the line containing the
 *     trailing `}` of the init body, column 0).
 *   - No init: synthesize full `init(<f>: <T>) { self.<f> = <expr> }`
 *     block at the line right after the object's `{`.
 *
 * Methods (including init) are HOISTED to top-level program->decls[]
 * as IRON_NODE_METHOD_DECL nodes per Phase 9 D-06/D-07 — Iron_ObjectDecl
 * has NO methods[] field. Same-file containment is verified via
 * arena-interned filename pointer-equality + line-range containment
 * inside the object decl's span.
 *
 * First Phase 12 consumer of the edit_text_edits[] array shape (Plan
 * 12-01 D-27); both edits emitted in document order so the LSP client's
 * undo stack reads naturally.
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/edit/codeaction/codeaction_indent.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* type_ann_to_string — small Iron_TypeAnnotation walker; arena-allocates
 * result. Mirrors src/parser/printer.c:185-200; co-located here as the
 * sole Phase 12 consumer (lifting to a shared helper deferred — second
 * consumer is quickfix_object_no_init.c; lift threshold is the next
 * phase that adds a 3rd; tracked as DEF-12-15). T-12-03-01 mitigation:
 * bounded buf[256] + size checks prevent runaway recursion. */
static const char *type_ann_to_string(const Iron_Node *type_ann,
                                         Iron_Arena      *a) {
    if (!type_ann || type_ann->kind != IRON_NODE_TYPE_ANNOTATION) return NULL;
    const Iron_TypeAnnotation *t = (const Iron_TypeAnnotation *)type_ann;
    if (!t->name) return NULL;
    char buf[256];
    size_t n = 0;
    if (t->is_array) {
        if (n + 2 >= sizeof(buf)) return NULL;
        buf[n++] = '['; buf[n++] = ']';
    }
    size_t nl = strlen(t->name);
    if (n + nl >= sizeof(buf)) return NULL;
    memcpy(buf + n, t->name, nl);
    n += nl;
    if (t->generic_arg_count > 0) {
        if (n + 1 >= sizeof(buf)) return NULL;
        buf[n++] = '<';
        for (int i = 0; i < t->generic_arg_count; i++) {
            if (i > 0) {
                if (n + 2 >= sizeof(buf)) return NULL;
                buf[n++] = ','; buf[n++] = ' ';
            }
            const char *child = type_ann_to_string(t->generic_args[i], a);
            if (!child) return NULL;
            size_t cl = strlen(child);
            if (n + cl >= sizeof(buf)) return NULL;
            memcpy(buf + n, child, cl);
            n += cl;
        }
        if (n + 1 >= sizeof(buf)) return NULL;
        buf[n++] = '>';
    }
    if (t->is_nullable) {
        if (n + 1 >= sizeof(buf)) return NULL;
        buf[n++] = '?';
    }
    return iron_arena_strdup(a, buf, n);
}

/* Find the Iron_ObjectDecl whose 1-indexed span line range covers
 * `diag->span.line`. Walks program->decls[] linearly (mirror of
 * patch_lookup.c:36-52). Returns NULL when no enclosing object exists. */
static const Iron_ObjectDecl *find_object_for_diag(const Iron_Program  *program,
                                                      const Iron_Diagnostic *diag) {
    if (!program || !diag) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        const Iron_ObjectDecl *od = (const Iron_ObjectDecl *)d;
        if (diag->span.line >= od->span.line &&
            diag->span.line <= od->span.end_line) {
            return od;
        }
    }
    return NULL;
}

/* Find the Iron_Field on `od` whose span line range covers `diag->span.line`.
 * The field span ends at the type-annotation; the diagnostic at parser.c:3505
 * fires on the `=` token which is on the same line as the field name in
 * the canonical form. Returns NULL on miss. */
static const Iron_Field *find_field_for_diag(const Iron_ObjectDecl *od,
                                                const Iron_Diagnostic *diag) {
    if (!od || !diag) return NULL;
    for (int j = 0; j < od->field_count; j++) {
        const Iron_Node *fnode = od->fields[j];
        if (!fnode || fnode->kind != IRON_NODE_FIELD) continue;
        const Iron_Field *f = (const Iron_Field *)fnode;
        /* Field span covers `name: T` — the `= expr` portion was consumed
         * for recovery and is NOT covered by f->span. The diagnostic span
         * sits ON the `=` token which lives between the field's end_col
         * and end-of-line. Use the field's start line as the anchor. */
        if (diag->span.line >= f->span.line &&
            diag->span.line <= (f->span.end_line == 0
                                  ? f->span.line : f->span.end_line)) {
            return f;
        }
        /* Tolerance: the `=` may sit on the line right after the field's
         * type annotation (the field span end_line). Match if diag->line
         * == f->span.end_line + 1 too. */
        if (f->span.end_line != 0 &&
            diag->span.line == f->span.end_line + 1) {
            /* But only if no other field starts before then. */
            return f;
        }
    }
    return NULL;
}

/* Walk program->decls[] for an init MethodDecl whose span sits inside the
 * given object decl's span (line containment) AND whose type_name matches
 * od->name. Returns the first match (declaration order) or NULL. */
static const Iron_MethodDecl *find_init_method_in_object(
    const Iron_Program    *program,
    const Iron_ObjectDecl *od)
{
    if (!program || !od) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        const Iron_MethodDecl *m = (const Iron_MethodDecl *)d;
        if (!m->is_init) continue;
        if (!m->type_name || !od->name) continue;
        if (strcmp(m->type_name, od->name) != 0) continue;
        /* Same-file gate via arena-interned filename pointer equality. */
        if (m->span.filename != od->span.filename) continue;
        /* Line-range containment inside the object decl. */
        if (m->span.line < od->span.line) continue;
        if (m->span.end_line > od->span.end_line) continue;
        return m;
    }
    return NULL;
}

/* Re-tokenize the document slice from start_byte..end_byte (exclusive)
 * and locate the first IRON_TOK_ASSIGN token. Returns its (line, col)
 * 1-indexed in the GLOBAL document coordinate space (the lexer reports
 * positions within its slice; we offset by anchor_line/anchor_col).
 *
 * Returns true on success. NULL-safe; returns false on lex failure or
 * no `=` token in the slice. */
static bool find_assign_in_field_slice(const struct IronLsp_Document *doc,
                                          Iron_Arena                    *arena,
                                          const Iron_Field              *f,
                                          uint32_t                      *out_line,
                                          uint32_t                      *out_col)
{
    if (!doc || !arena || !f) return false;
    /* Slice: from start of f's start line through start of (end_line + 8)
     * to be safe — the `=` may sit a few lines after a multi-line type
     * annotation. Bound by doc->text_len. */
    uint32_t start_line0 = (f->span.line > 0) ? f->span.line - 1 : 0;
    uint32_t end_line0   = (f->span.end_line > 0) ? f->span.end_line - 1
                                                   : start_line0;
    /* Add a 4-line tolerance for unusual layouts; cap at doc end. */
    end_line0 += 4;
    size_t slice_start = ilsp_byte_of_line(&doc->line_idx, start_line0);
    size_t slice_end   = ilsp_byte_of_line(&doc->line_idx, end_line0 + 1);
    if (slice_end > doc->text_len) slice_end = doc->text_len;
    if (slice_start >= slice_end || slice_start >= doc->text_len) return false;
    size_t slice_len = slice_end - slice_start;

    /* Copy the slice into an arena buffer + NUL-terminate. The lexer wants
     * a NUL-terminated source for safety; doc->text is not guaranteed
     * NUL-terminated past text_len. */
    char *slice = (char *)iron_arena_alloc(arena, slice_len + 1, 1);
    if (!slice) return false;
    memcpy(slice, doc->text + slice_start, slice_len);
    slice[slice_len] = '\0';

    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer    lex   = iron_lexer_create(slice, "<qf03-slice>", arena, &diags);
    Iron_Token   *toks  = iron_lex_all(&lex);
    if (!toks) {
        iron_diaglist_free(&diags);
        return false;
    }

    bool found = false;
    /* stb_ds dynamic array: count via header (arrlen). Use a manual loop
     * over EOF-terminated form: stop on IRON_TOK_EOF. */
    for (size_t i = 0; toks[i].kind != IRON_TOK_EOF; i++) {
        if (toks[i].kind == IRON_TOK_ASSIGN) {
            /* Lexer token line/col is 1-indexed within the SLICE. The
             * slice starts at `start_line0` (0-indexed) which is
             * f->span.line - 1 in 1-indexed source coords. So global
             * 1-indexed line = start_line0 + token.line. Global col is
             * unchanged because the slice begins at column 0 of
             * start_line0. */
            *out_line = start_line0 + toks[i].line;  /* 1-indexed global */
            *out_col  = toks[i].col;                 /* 1-indexed global */
            found = true;
            break;
        }
    }
    iron_diaglist_free(&diags);
    return found;
}

/* ── Entry point ─────────────────────────────────────────────────── */

void ilsp_quickfix_v3_inline_default(const Iron_Diagnostic           *diag,
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
    if (out_cap == 0) return;
    memset(&out_arr[0], 0, sizeof(out_arr[0]));
    if (!diag || !doc || !arena) return;

    /* Re-analyze: own per-call arena + diaglist; caller's arena reserved
     * for output strings only. */
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

    const Iron_ObjectDecl *od = find_object_for_diag(program, diag);
    if (!od) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    const Iron_Field *f = find_field_for_diag(od, diag);
    if (!f || !f->name || !f->type_ann) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Locate the `=` token via re-tokenization of a doc slice. */
    uint32_t eq_line_1 = 0, eq_col_1 = 0;
    if (!find_assign_in_field_slice(doc, arena, f, &eq_line_1, &eq_col_1)) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    /* Convert to LSP 0-indexed coordinates. */
    uint32_t eq_line_0 = eq_line_1 - 1;
    uint32_t eq_col_0  = eq_col_1 - 1;

    /* Compute the byte offset of the `=` token, then expand backwards
     * to swallow any preceding whitespace (so we delete `space-eq-space-rhs`
     * and leave `name: Type` cleanly). */
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, eq_line_0);
    size_t eq_byte    = line_start + (size_t)eq_col_0;
    if (eq_byte > doc->text_len) eq_byte = doc->text_len;

    /* Walk back over whitespace before the `=`. */
    size_t del_start_byte = eq_byte;
    while (del_start_byte > line_start &&
           (doc->text[del_start_byte - 1] == ' ' ||
            doc->text[del_start_byte - 1] == '\t')) {
        del_start_byte--;
    }
    uint32_t del_start_line = eq_line_0;
    uint32_t del_start_col  = (uint32_t)(del_start_byte - line_start);

    /* The deletion runs from del_start_byte to end-of-line (the `=` and
     * the entire RHS expression occupy the rest of the line in canonical
     * Iron formatting). Compute end-of-line via the line index. */
    size_t next_line_start = ilsp_byte_of_line(&doc->line_idx, eq_line_0 + 1);
    if (next_line_start > doc->text_len) next_line_start = doc->text_len;
    /* Exclude the trailing newline byte from the deletion so the field
     * still ends at column 0 of the next line — matches the canonical
     * Iron formatting convention. */
    size_t del_end_byte = next_line_start;
    if (del_end_byte > del_start_byte &&
        del_end_byte > 0 &&
        doc->text[del_end_byte - 1] == '\n') {
        del_end_byte--;
    }
    uint32_t del_end_line = eq_line_0;
    uint32_t del_end_col  = (uint32_t)(del_end_byte - line_start);

    /* Extract the RHS expression text: from byte-after-`=` through del_end_byte;
     * trim leading + trailing whitespace. */
    size_t rhs_start_byte = eq_byte + 1;  /* skip `=` */
    while (rhs_start_byte < del_end_byte &&
           (doc->text[rhs_start_byte] == ' ' ||
            doc->text[rhs_start_byte] == '\t')) {
        rhs_start_byte++;
    }
    if (rhs_start_byte >= del_end_byte) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    size_t rhs_len = del_end_byte - rhs_start_byte;
    /* Trim trailing whitespace from RHS. */
    while (rhs_len > 0 &&
           (doc->text[rhs_start_byte + rhs_len - 1] == ' ' ||
            doc->text[rhs_start_byte + rhs_len - 1] == '\t' ||
            doc->text[rhs_start_byte + rhs_len - 1] == '\r')) {
        rhs_len--;
    }
    if (rhs_len == 0) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    char *rhs_text = (char *)iron_arena_alloc(arena, rhs_len + 1, 1);
    if (!rhs_text) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    memcpy(rhs_text, doc->text + rhs_start_byte, rhs_len);
    rhs_text[rhs_len] = '\0';

    /* Allocate the 2-edit array on the caller's arena. */
    IronLsp_TextEdit *edits = (IronLsp_TextEdit *)iron_arena_alloc(
        arena, 2 * sizeof(*edits), alignof(IronLsp_TextEdit));
    if (!edits) {
        iron_diaglist_free(&walk_diags);
        iron_arena_free(&walk_arena);
        return;
    }
    memset(edits, 0, 2 * sizeof(*edits));

    /* Edit A: deletion of ` = <expr>` from end of `name: Type`. */
    edits[0].start_line = del_start_line;
    edits[0].start_char = del_start_col;
    edits[0].end_line   = del_end_line;
    edits[0].end_char   = del_end_col;
    edits[0].new_text   = "";

    /* Indent derivation. Use object body span to derive outer (one nesting
     * level inside object); body_indent = outer + 4. */
    uint32_t outer_indent = ilsp_codeaction_derive_body_indent(
        doc,
        (uint32_t)(od->span.line),
        (uint32_t)(od->span.end_line - 1));
    if (outer_indent == 0 || outer_indent > 64) outer_indent = 4;
    uint32_t body_indent = outer_indent + 4;

    /* Edit B: existing init or synthesize. */
    const Iron_MethodDecl *im = find_init_method_in_object(program, od);
    if (im && im->body) {
        /* Append before init's closing `}`. The body span ends on the
         * line containing `}`; insert at column 0 of that line so the
         * new `self.x = expr\n` line precedes the `}` line, then the
         * `}` line stays where it was. */
        if (im->body->span.end_line == 0) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }
        size_t need = rhs_len + strlen(f->name) + (size_t)body_indent + 32;
        char *append_text = (char *)iron_arena_alloc(arena, need, 1);
        if (!append_text) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }
        int written = snprintf(append_text, need,
            "%*sself.%s = %s\n", (int)body_indent, "", f->name, rhs_text);
        if (written < 0 || (size_t)written >= need) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }
        /* LSP 0-indexed line of init body's closing-`}` line. */
        uint32_t close_line_0 = (uint32_t)(im->body->span.end_line - 1);
        edits[1].start_line = close_line_0;
        edits[1].start_char = 0;
        edits[1].end_line   = close_line_0;
        edits[1].end_char   = 0;
        edits[1].new_text   = append_text;
    } else {
        /* Synthesize new init block. */
        const char *type_text = type_ann_to_string(f->type_ann, arena);
        if (!type_text) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }
        size_t need = strlen(type_text) + (size_t)strlen(f->name) * 3 +
                      rhs_len + (size_t)outer_indent * 2 + 64;
        char *block = (char *)iron_arena_alloc(arena, need, 1);
        if (!block) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }
        int written = snprintf(block, need,
            "%*sinit(%s: %s) {\n%*sself.%s = %s\n%*s}\n",
            (int)outer_indent, "", f->name, type_text,
            (int)body_indent, "", f->name, rhs_text,
            (int)outer_indent, "");
        if (written < 0 || (size_t)written >= need) {
            iron_diaglist_free(&walk_diags);
            iron_arena_free(&walk_arena);
            return;
        }
        /* Insertion point: zero-width at the line right after the
         * object's `{`. Object span.line is the line containing
         * `object Foo {` (1-indexed); 0-indexed line right after that
         * is `(span.line - 1) + 1 = span.line`. */
        edits[1].start_line = (uint32_t)od->span.line;
        edits[1].start_char = 0;
        edits[1].end_line   = (uint32_t)od->span.line;
        edits[1].end_char   = 0;
        edits[1].new_text   = block;
    }

    out_arr[0].title             = "Move default into init(...)";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = true;
    out_arr[0].edit_text_edits   = edits;
    out_arr[0].edit_text_edits_n = 2;
    *out_n = 1;

    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
