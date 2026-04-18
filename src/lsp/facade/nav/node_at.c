/* Phase 3 Plan 01 Task 04 (NAV-16) -- cursor -> AST node lookup.
 *
 * Walks the top-level decls of a sealed Iron_Program, finds the first
 * span covering the (line, col) position derived from the LSP Position,
 * then descends into struct-embedded child lists (object fields,
 * interface method_sigs, enum variants) to find the innermost node.
 * Expression-body walking is intentionally simple at this phase -- the
 * primitive returns the decl node whose span covers the cursor, which
 * is what every NAV endpoint in Plans 02..06 actually needs. */

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

/* Find the innermost child node of `parent` whose span covers (line, col).
 * If no child covers, returns `parent`. Silently skips IRON_NODE_ERROR. */
static Iron_Node *descend_into(Iron_Node *parent,
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
            /* Func / method / import / value / other -- we do not
             * descend into expression bodies in Phase 3 Plan 01; the
             * enclosing decl is the answer the NAV endpoints need. */
            break;
    }
    return parent;
}

Iron_Node *ilsp_nav_node_at(const IronLsp_Document   *doc,
                             const Iron_Program       *program,
                             IronLsp_Position          pos,
                             IronLsp_PositionEncoding  enc) {
    if (!doc || !program) return NULL;
    uint32_t line = 0, col = 0;
    if (!position_to_iron_line_col(doc, pos, enc, &line, &col)) return NULL;

    /* Scan top-level decls for the one whose span covers (line, col). */
    Iron_Node *covering = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        if (span_covers(&d->span, line, col)) {
            covering = d;
            break;
        }
    }
    if (!covering) return NULL;  /* cursor is in whitespace */

    /* Descend once into decl-level children. */
    Iron_Node *inner = descend_into(covering, line, col);
    return inner ? inner : covering;
}
