/* Phase 3 Plan 01 Task 04 (NAV-16) -- cursor -> AST node lookup.
 *
 * Walks the top-level decls of a sealed Iron_Program, finds the first
 * span covering the (line, col) position derived from the LSP Position,
 * then walks it (iron_ast_walk: fields, signatures, bodies, expressions,
 * type annotations) for the innermost node under the cursor. */

#include "lsp/facade/nav/node_at.h"

#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/utf.h"
#include "lsp/facade/types.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Convert an LSP Position to a 1-based (line, col) pair matching the
 * Iron_Span encoding. Returns true on success; false when the line is
 * out of range. */
static bool position_to_iron_line_col(const IronLsp_Document   *doc,
                                       IronLsp_Position          pos,
                                       IronLsp_PositionEncoding  enc,
                                       uint32_t                 *out_line,
                                       uint32_t                 *out_col) {
    if (!doc || !doc->text) return false;
    /* Iron_Span uses 1-based lines/cols; LSP Positions are 0-based. */
    uint32_t line0 = pos.line;

    /* Resolve line's byte range from the line index. */
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, line0);
    /* Fetch next-line start (or text_len) to get the line length. */
    size_t next_start;
    size_t line_count = 0;
    if (doc->line_idx.starts) {
        /* stb_ds arrlen is spelled inline here because including stb_ds
         * into a header-only path is heavy; document.c already maintains
         * starts[] and we just need its length. Use the documented contract
         * that the last entry covers the final newline: iterate until a
         * start > line_start OR we run out. */
        size_t i = line0 + 1;
        /* starts is stb_ds dynamic; peek via the header-less pattern:
         * stb_ds stores length in a header BEFORE the pointer. To avoid
         * depending on internals we fall back to a linear scan that is
         * bounded by the doc text size. */
        (void)i;
        /* Simpler path: walk forward from line_start to the next '\n'. */
        next_start = line_start;
        while (next_start < doc->text_len && doc->text[next_start] != '\n') {
            next_start++;
        }
    } else {
        next_start = doc->text_len;
    }
    (void)line_count;

    if (line_start > doc->text_len) return false;
    if (next_start > doc->text_len) next_start = doc->text_len;
    size_t line_len = next_start - line_start;
    const char *line_text = doc->text + line_start;

    /* Map pos.character -> byte offset within the line according to the
     * negotiated encoding. */
    size_t byte_in_line;
    if (enc == ILSP_ENC_UTF16) {
        byte_in_line = ilsp_utf16_column_to_utf8_byte(line_text, line_len,
                                                       pos.character);
    } else {
        byte_in_line = ilsp_utf8_column_to_utf8_byte(line_text, line_len,
                                                      pos.character);
    }
    if (byte_in_line > line_len) byte_in_line = line_len;

    *out_line = line0 + 1;
    *out_col  = (uint32_t)byte_in_line + 1;
    return true;
}

/* True if the 1-based (line, col) position is within span (inclusive
 * on both ends, matching Iron_Span semantics where end_col is the last
 * byte of the covered token run). */
static bool span_covers(const Iron_Span *sp, uint32_t line, uint32_t col) {
    if (!sp) return false;
    if (line < sp->line || line > sp->end_line) return false;
    if (line == sp->line && col < sp->col) return false;
    if (line == sp->end_line && col > sp->end_col) return false;
    return true;
}

/* The member of `parent` (object field, interface signature, enum variant)
 * whose span covers (line, col), else `parent`. Skips IRON_NODE_ERROR. */
static Iron_Node *descend_members(Iron_Node *parent,
                                uint32_t line, uint32_t col) {
    if (!parent) return NULL;
    switch ((int)parent->kind) {
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *o = (Iron_ObjectDecl *)parent;
            for (int i = 0; i < o->field_count; i++) {
                Iron_Node *c = o->fields[i];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                if (span_covers(&c->span, line, col)) return c;
            }
            break;
        }
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *ifc = (Iron_InterfaceDecl *)parent;
            for (int i = 0; i < ifc->method_count; i++) {
                Iron_Node *c = ifc->method_sigs[i];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                if (span_covers(&c->span, line, col)) return c;
            }
            break;
        }
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *e = (Iron_EnumDecl *)parent;
            for (int i = 0; i < e->variant_count; i++) {
                Iron_Node *c = e->variants[i];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                if (span_covers(&c->span, line, col)) return c;
            }
            break;
        }
        default:
            /* Func / method / import / value / other: the decl. */
            break;
    }
    return parent;
}

/* True when span `in` lies within span `out` (equal spans included). */
static bool span_within(const Iron_Span *in, const Iron_Span *out) {
    return span_covers(out, in->line, in->col) &&
           span_covers(out, in->end_line, in->end_col);
}

typedef struct {
    uint32_t   line, col;
    Iron_Node *best;
} InnermostCtx;

/* Pre-order: a child is visited after its parent, so `within` (which
 * admits equal spans) lets the deeper node win a tie. Children are walked
 * even when the parent does not cover the cursor: a few parents' spans
 * stop short of their last child (a call's span may end at its callee). */
static bool innermost_visit(Iron_Visitor *v, Iron_Node *n) {
    InnermostCtx *c = (InnermostCtx *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->span.line == 0) return true;  /* synthesized, no position */
    if (span_covers(&n->span, c->line, c->col) &&
        (!c->best || span_within(&n->span, &c->best->span))) {
        c->best = n;
    }
    return true;
}

/* The innermost node under (line, col) inside `decl`: an identifier,
 * a method call or field access (cursor on the member name), a type
 * annotation, a literal, a binding... or `decl` itself when the cursor is
 * on its name or keywords. */
static Iron_Node *descend_into(Iron_Node *decl, uint32_t line, uint32_t col) {
    InnermostCtx c = { line, col, NULL };
    Iron_Visitor v = { .ctx = &c, .visit_node = innermost_visit, .post_visit = NULL };
    iron_ast_walk(decl, &v);
    return c.best;
}

/* The smallest top-level decl covering the cursor; sets *line / *col. */
static Iron_Node *covering_decl(const IronLsp_Document   *doc,
                                const Iron_Program       *program,
                                IronLsp_Position          pos,
                                IronLsp_PositionEncoding  enc,
                                uint32_t                 *out_line,
                                uint32_t                 *out_col) {
    if (!doc || !program) return NULL;
    uint32_t line = 0, col = 0;
    if (!position_to_iron_line_col(doc, pos, enc, &line, &col)) return NULL;
    *out_line = line;
    *out_col = col;

    /* Scan top-level decls for the one whose span covers (line, col).
     *
     * Phase 9 D-06/D-07 (NAV-16 v3 walker descent): the parser hoists v3
     * method-in-block methods (regular methods, init / named init, and
     * patch methods) to top-level Iron_Program decls via extra_decls_out
     * (see src/parser/parser.c:3331-3460). The hoisted MethodDecl span
     * is fully nested inside the source object's OBJECT_DECL span. A
     * naive "first-covering decl wins" scan returns the OBJECT_DECL,
     * which is too coarse for cursor-in-method-body queries. We pick the
     * smallest covering span instead — that yields the hoisted
     * MethodDecl when it nests inside the object body, while still
     * returning the OBJECT_DECL for cursor positions inside the object's
     * fields[]/decl-header but outside any method body. */
    Iron_Node *covering = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        if (!span_covers(&d->span, line, col)) continue;
        if (covering == NULL) {
            covering = d;
            continue;
        }
        /* Pick the more specific (smaller) span. */
        if (span_covers(&covering->span,
                         d->span.line, d->span.col) &&
            span_covers(&covering->span,
                         d->span.end_line, d->span.end_col)) {
            covering = d;
        }
    }
    return covering;  /* NULL: the cursor is in whitespace */
}

Iron_Node *ilsp_nav_node_at(const IronLsp_Document   *doc,
                             const Iron_Program       *program,
                             IronLsp_Position          pos,
                             IronLsp_PositionEncoding  enc) {
    uint32_t line = 0, col = 0;
    Iron_Node *covering = covering_decl(doc, program, pos, enc, &line, &col);
    if (!covering) return NULL;
    Iron_Node *inner = descend_into(covering, line, col);
    return inner ? inner : covering;
}

Iron_Node *ilsp_nav_decl_at(const IronLsp_Document   *doc,
                             const Iron_Program       *program,
                             IronLsp_Position          pos,
                             IronLsp_PositionEncoding  enc) {
    uint32_t line = 0, col = 0;
    Iron_Node *covering = covering_decl(doc, program, pos, enc, &line, &col);
    if (!covering) return NULL;
    return descend_members(covering, line, col);
}
