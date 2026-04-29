/* Phase 12 Plan 12-02 (QF-02) — quickfix for IRON_ERR_V3_NO_INIT (264).
 *
 * Recipe (D-22, D-23, D-25):
 *   title         = "Synthesize default init(...)"
 *   kind          = "quickfix"
 *   is_preferred  = true (mechanical fix; user clearly intended an init)
 *   edit          = zero-width insertion at line after the object's `{`
 *   newText       = "<indent>init(<f>: <T>, ...) {\n
 *                    <body_indent>self.<f> = <f>\n...\n<indent>}\n"
 *
 * Walks Iron_ObjectDecl.fields[] (Iron_Node** of IRON_NODE_FIELD); filters
 * `is_var == true` to mirror the parser's E0264 emit guard at parser.c:3993
 * (var_field_count > 0). Renders type via a small Iron_TypeAnnotation
 * walker mirroring src/parser/printer.c:185-200 (RESEARCH Open Item #3:
 * type_ann is a node, not a resolved Iron_Type — co-located walker
 * deferred to avoid lifting a single consumer).
 */

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/edit/codeaction/codeaction_indent.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* type_ann_to_string — small Iron_TypeAnnotation walker; arena-allocates
 * result. Mirrors src/parser/printer.c:185-200; co-located here as the
 * sole Phase 12 consumer (lifting to a shared helper deferred). Returns
 * NULL on OOM, malformed input, or buffer overflow. T-12-02-04
 * mitigation: bounded buf[256] + size checks prevent runaway recursion. */
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
 * `diag->span.line`. The diagnostic emit at parser.c:4011 fires on the
 * object's name token, so line containment is sufficient. Returns NULL
 * when no enclosing object decl exists. */
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

void ilsp_quickfix_object_no_init(const Iron_Diagnostic           *diag,
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

    /* Re-analyze the document to obtain a fresh Iron_Program. The
     * compile facade owns its own per-call arena + diaglist; the
     * caller's arena is reserved for the action's output strings. */
    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags      = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = NULL };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &diags);
    if (!program) {
        iron_diaglist_free(&diags);
        iron_arena_free(&walk_arena);
        return;
    }

    const Iron_ObjectDecl *od = find_object_for_diag(program, diag);
    if (!od || od->field_count <= 0 || !od->fields) {
        iron_diaglist_free(&diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Indent derivation: outer = body content's indent (one nesting level
     * deep below the object header); body_indent = outer + 4 (one more
     * level). span.line is 1-indexed; derive_body_indent expects 0-indexed
     * line numbers. */
    uint32_t outer_indent = ilsp_codeaction_derive_body_indent(
        doc,
        (uint32_t)(od->span.line),                /* line right after `{` */
        (uint32_t)(od->span.end_line - 1));       /* up to (not incl) `}` */
    if (outer_indent == 0 || outer_indent > 64) outer_indent = 4;
    uint32_t body_indent = outer_indent + 4;

    /* Build params + body via bounded snprintf into arena buffers.
     * cap = 1024 keeps QF-02 work bounded (T-12-02-04 mitigation). */
    const size_t cap = 1024;
    char *params = (char *)iron_arena_alloc(arena, cap, 1);
    char *body   = (char *)iron_arena_alloc(arena, cap, 1);
    if (!params || !body) {
        iron_diaglist_free(&diags);
        iron_arena_free(&walk_arena);
        return;
    }
    params[0] = '\0';
    body[0]   = '\0';

    size_t pn = 0, bn = 0;
    bool first = true;
    for (int j = 0; j < od->field_count; j++) {
        const Iron_Node *fnode = od->fields[j];
        if (!fnode || fnode->kind != IRON_NODE_FIELD) continue;
        const Iron_Field *f = (const Iron_Field *)fnode;
        if (!f->is_var) continue;        /* var-only — RESEARCH Open Item #4 */
        if (!f->name || !f->type_ann) {  /* parser recovery state */
            iron_diaglist_free(&diags);
            iron_arena_free(&walk_arena);
            return;
        }
        const char *type_text = type_ann_to_string(f->type_ann, arena);
        if (!type_text) {
            iron_diaglist_free(&diags);
            iron_arena_free(&walk_arena);
            return;
        }
        int written = snprintf(params + pn, cap - pn,
            "%s%s: %s", first ? "" : ", ", f->name, type_text);
        if (written < 0 || (size_t)written >= cap - pn) {
            iron_diaglist_free(&diags);
            iron_arena_free(&walk_arena);
            return;
        }
        pn += (size_t)written;
        written = snprintf(body + bn, cap - bn,
            "%*sself.%s = %s\n",
            (int)body_indent, "", f->name, f->name);
        if (written < 0 || (size_t)written >= cap - bn) {
            iron_diaglist_free(&diags);
            iron_arena_free(&walk_arena);
            return;
        }
        bn += (size_t)written;
        first = false;
    }
    if (first) {
        /* No `var` fields — defensive (E0264 only fires when var_field_count
         * > 0; reaching here means the AST is in an inconsistent state). */
        iron_diaglist_free(&diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Assemble the full insertion text:
     *   "<outer_indent>init(<params>) {\n
     *    <body><outer_indent>}\n" */
    size_t need = pn + bn + (size_t)outer_indent * 2 + 64;
    char *new_text = (char *)iron_arena_alloc(arena, need, 1);
    if (!new_text) {
        iron_diaglist_free(&diags);
        iron_arena_free(&walk_arena);
        return;
    }
    int total = snprintf(new_text, need,
        "%*sinit(%s) {\n%s%*s}\n",
        (int)outer_indent, "",
        params,
        body,
        (int)outer_indent, "");
    if (total < 0 || (size_t)total >= need) {
        iron_diaglist_free(&diags);
        iron_arena_free(&walk_arena);
        return;
    }

    /* Insertion point: zero-width at line AFTER the object's `{`. The
     * Iron span uses 1-indexed lines; LSP Range uses 0-indexed lines.
     * Object decl `span.line` = the line containing `object Foo {`
     * (1-indexed); the line right after that in 0-indexed terms is
     * `(span.line - 1) + 1 = span.line`. The +1 / -1 cancel. */
    out_arr[0].title             = "Synthesize default init(...)";
    out_arr[0].kind              = "quickfix";
    out_arr[0].originating_diag  = diag;
    out_arr[0].is_preferred      = true;
    out_arr[0].edit_start_line   = (uint32_t)od->span.line;
    out_arr[0].edit_start_char   = 0;
    out_arr[0].edit_end_line     = (uint32_t)od->span.line;
    out_arr[0].edit_end_char     = 0;
    out_arr[0].edit_new_text     = new_text;
    *out_n = 1;

    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
}
