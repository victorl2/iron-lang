/* Phase 3 Plan 03 Task 03 (NAV-07, D-10) -- textDocument/documentSymbol.
 *
 * Hierarchical DocumentSymbol tree. Walks program->decls in source
 * order; for each top-level decl builds an IronLsp_DocSymbol with LSP
 * SymbolKind mapping (D-10), recursing into object fields + methods,
 * interface method_sigs, and enum variants.
 *
 * Ranges:
 *   range           = full decl span (the parser already extends the
 *                      span to include attached doc_comment lines).
 *   selectionRange  = identifier-only span (falls back to full span
 *                      when a separate ident token is unavailable).
 *
 * Hierarchical vs flat: when `hierarchical == false`, children are
 * still materialised in the tree (Plan 03 always emits hierarchical
 * structure; the handler layer is responsible for flattening if the
 * client lacks the capability bit). */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* LSP SymbolKind codes (from spec). */
#define LSP_SYMKIND_FILE        1
#define LSP_SYMKIND_MODULE      2
#define LSP_SYMKIND_PACKAGE     4
#define LSP_SYMKIND_CLASS       5
#define LSP_SYMKIND_METHOD      6
#define LSP_SYMKIND_PROPERTY    7
#define LSP_SYMKIND_FIELD       8
#define LSP_SYMKIND_ENUM        10
#define LSP_SYMKIND_INTERFACE   11
#define LSP_SYMKIND_FUNCTION    12
#define LSP_SYMKIND_VARIABLE    13
#define LSP_SYMKIND_CONSTANT    14
#define LSP_SYMKIND_ENUMMEMBER  22

/* Owned-by-arena strdup. */
static const char *astrdup(Iron_Arena *a, const char *s) {
    if (!s) return NULL;
    return iron_arena_strdup(a, s, strlen(s));
}

/* Build a DocSymbol for a field within an object decl. */
static IronLsp_DocSymbol build_field(Iron_Field *f,
                                       IronLsp_Document *doc,
                                       IronLsp_PositionEncoding enc,
                                       Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, f->name);
    s.kind  = LSP_SYMKIND_FIELD;
    s.range           = ilsp_span_to_lsp_range(f->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

/* Build DocSymbol for an enum variant. */
static IronLsp_DocSymbol build_variant(Iron_EnumVariant *v,
                                         IronLsp_Document *doc,
                                         IronLsp_PositionEncoding enc,
                                         Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, v->name);
    s.kind  = LSP_SYMKIND_ENUMMEMBER;
    s.range           = ilsp_span_to_lsp_range(v->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

/* Build DocSymbol for an interface method sig (MethodDecl). */
static IronLsp_DocSymbol build_method_sig(Iron_Node *n,
                                             IronLsp_Document *doc,
                                             IronLsp_PositionEncoding enc,
                                             Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    /* Interface method sigs are parsed as FuncDecl-shaped structural
     * helpers; grab the name via dispatch on kind. */
    const char *nm = NULL;
    if (n->kind == IRON_NODE_FUNC_DECL)   nm = ((Iron_FuncDecl *)n)->name;
    if (n->kind == IRON_NODE_METHOD_DECL) nm = ((Iron_MethodDecl *)n)->method_name;
    s.name  = nm ? astrdup(arena, nm) : "";
    s.kind  = LSP_SYMKIND_METHOD;
    s.range           = ilsp_span_to_lsp_range(n->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

/* Build DocSymbol for top-level object, recursing into fields + methods. */
static IronLsp_DocSymbol build_object(Iron_ObjectDecl *o,
                                        IronLsp_Document *doc,
                                        IronLsp_PositionEncoding enc,
                                        Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, o->name);
    s.kind  = LSP_SYMKIND_CLASS;
    s.range           = ilsp_span_to_lsp_range(o->span, doc, enc);
    s.selection_range = s.range;

    /* Fields. Methods on objects are represented as separate top-level
     * Iron_MethodDecl nodes in the program; they're attached in the
     * caller pass when matching type_name. */
    if (o->field_count > 0) {
        IronLsp_DocSymbol *kids = (IronLsp_DocSymbol *)iron_arena_alloc(
            arena, sizeof(IronLsp_DocSymbol) * (size_t)o->field_count,
            _Alignof(IronLsp_DocSymbol));
        if (kids) {
            for (int i = 0; i < o->field_count; i++) {
                Iron_Node *c = o->fields[i];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                if (c->kind == IRON_NODE_FIELD) {
                    kids[i] = build_field((Iron_Field *)c, doc, enc, arena);
                } else {
                    memset(&kids[i], 0, sizeof(kids[i]));
                    kids[i].name = "";
                    kids[i].kind = LSP_SYMKIND_FIELD;
                    kids[i].range = ilsp_span_to_lsp_range(c->span, doc, enc);
                    kids[i].selection_range = kids[i].range;
                }
            }
            s.children = kids;
            s.child_count = (size_t)o->field_count;
        }
    }
    return s;
}

static IronLsp_DocSymbol build_interface(Iron_InterfaceDecl *i,
                                           IronLsp_Document *doc,
                                           IronLsp_PositionEncoding enc,
                                           Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, i->name);
    s.kind  = LSP_SYMKIND_INTERFACE;
    s.range           = ilsp_span_to_lsp_range(i->span, doc, enc);
    s.selection_range = s.range;

    if (i->method_count > 0) {
        IronLsp_DocSymbol *kids = (IronLsp_DocSymbol *)iron_arena_alloc(
            arena, sizeof(IronLsp_DocSymbol) * (size_t)i->method_count,
            _Alignof(IronLsp_DocSymbol));
        if (kids) {
            for (int j = 0; j < i->method_count; j++) {
                Iron_Node *c = i->method_sigs[j];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                kids[j] = build_method_sig(c, doc, enc, arena);
            }
            s.children    = kids;
            s.child_count = (size_t)i->method_count;
        }
    }
    return s;
}

static IronLsp_DocSymbol build_enum(Iron_EnumDecl *e,
                                      IronLsp_Document *doc,
                                      IronLsp_PositionEncoding enc,
                                      Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, e->name);
    s.kind  = LSP_SYMKIND_ENUM;
    s.range           = ilsp_span_to_lsp_range(e->span, doc, enc);
    s.selection_range = s.range;

    if (e->variant_count > 0) {
        IronLsp_DocSymbol *kids = (IronLsp_DocSymbol *)iron_arena_alloc(
            arena, sizeof(IronLsp_DocSymbol) * (size_t)e->variant_count,
            _Alignof(IronLsp_DocSymbol));
        if (kids) {
            for (int j = 0; j < e->variant_count; j++) {
                Iron_Node *c = e->variants[j];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                if (c->kind == IRON_NODE_ENUM_VARIANT) {
                    kids[j] = build_variant((Iron_EnumVariant *)c, doc, enc, arena);
                }
            }
            s.children    = kids;
            s.child_count = (size_t)e->variant_count;
        }
    }
    return s;
}

static IronLsp_DocSymbol build_func(Iron_FuncDecl *f,
                                      IronLsp_Document *doc,
                                      IronLsp_PositionEncoding enc,
                                      Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, f->name);
    s.kind  = LSP_SYMKIND_FUNCTION;
    s.range           = ilsp_span_to_lsp_range(f->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

static IronLsp_DocSymbol build_method(Iron_MethodDecl *m,
                                        IronLsp_Document *doc,
                                        IronLsp_PositionEncoding enc,
                                        Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    /* Render as "Type.method" for the label. */
    const char *type_name   = m->type_name   ? m->type_name   : "";
    const char *method_name = m->method_name ? m->method_name : "";
    size_t la = strlen(type_name);
    size_t lb = strlen(method_name);
    char *buf = (char *)iron_arena_alloc(arena, la + lb + 2, 1);
    if (buf) {
        size_t o = 0;
        memcpy(buf + o, type_name, la); o += la;
        if (la > 0 && lb > 0) buf[o++] = '.';
        memcpy(buf + o, method_name, lb); o += lb;
        buf[o] = '\0';
        s.name = buf;
    } else {
        s.name = method_name;
    }
    s.kind  = LSP_SYMKIND_METHOD;
    s.range           = ilsp_span_to_lsp_range(m->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

static IronLsp_DocSymbol build_import(Iron_ImportDecl *i,
                                        IronLsp_Document *doc,
                                        IronLsp_PositionEncoding enc,
                                        Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, i->path ? i->path : "");
    s.kind  = LSP_SYMKIND_PACKAGE;
    s.range           = ilsp_span_to_lsp_range(i->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

static IronLsp_DocSymbol build_val(Iron_ValDecl *v,
                                     IronLsp_Document *doc,
                                     IronLsp_PositionEncoding enc,
                                     Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, v->name ? v->name : "");
    s.kind  = LSP_SYMKIND_CONSTANT;
    s.range           = ilsp_span_to_lsp_range(v->span, doc, enc);
    s.selection_range = s.range;
    return s;
}

void ilsp_facade_nav_document_symbol(IronLsp_Server       *server,
                                      IronLsp_Document     *doc,
                                      _Atomic bool         *cancel,
                                      Iron_Arena           *arena,
                                      IronLsp_DocSymbol   **out_syms,
                                      size_t               *out_n,
                                      bool                  hierarchical) {
    (void)hierarchical;  /* always build hierarchical; handler layer flattens */
    if (out_syms) *out_syms = NULL;
    if (out_n)    *out_n    = 0;
    if (!server || !doc || !arena || !out_syms || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version, .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    /* Count top-level non-error decls. */
    int count = 0;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        count++;
    }
    if (count <= 0) goto done;

    IronLsp_DocSymbol *arr = (IronLsp_DocSymbol *)iron_arena_alloc(
        arena, sizeof(IronLsp_DocSymbol) * (size_t)count,
        _Alignof(IronLsp_DocSymbol));
    if (!arr) goto done;

    size_t w = 0;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        switch ((int)d->kind) {
            case IRON_NODE_IMPORT_DECL:
                arr[w++] = build_import((Iron_ImportDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_OBJECT_DECL:
                arr[w++] = build_object((Iron_ObjectDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_INTERFACE_DECL:
                arr[w++] = build_interface((Iron_InterfaceDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_ENUM_DECL:
                arr[w++] = build_enum((Iron_EnumDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_FUNC_DECL:
                arr[w++] = build_func((Iron_FuncDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_METHOD_DECL:
                /* XXX_PHASE_10 - methods of v3 objects appear at top
                 * level of the documentSymbol tree rather than nested
                 * under their owning object, because the v3 parser
                 * hoists method-in-block (init / regular / patch) to
                 * top-level Iron_MethodDecl siblings. Cosmetic only;
                 * proper nesting requires a method-by-type-name index
                 * (~40 LoC) closer to feature work than plumbing.
                 * Phase 10 owns the fix (see RESEARCH.md §6 Q3). */
                arr[w++] = build_method((Iron_MethodDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_VAL_DECL:
                arr[w++] = build_val((Iron_ValDecl *)d, doc, enc, arena);
                break;
            default:
                /* Skip anything else (var decls at module level, stmt
                 * shapes, etc.). */
                break;
        }
    }

    *out_syms = arr;
    *out_n    = w;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
