/* Phase 3 Plan 03 Task 03 (NAV-07, D-10) -- textDocument/documentSymbol.
 *
 * Hierarchical DocumentSymbol tree. Walks program->decls in source
 * order; for each top-level decl builds an IronLsp_DocSymbol with LSP
 * SymbolKind mapping (D-10), recursing into object fields + methods,
 * interface method_sigs, and enum variants.
 *
 * Ranges:
 *   range           = full decl span.
 *   selectionRange  = the declaration's name (#362), always inside
 *                      range; the whole range when the name is not found.
 *
 * Methods declared in an object's block are children of the object; the
 * parser hoists them to top-level Iron_MethodDecl siblings, so they are
 * attached here by type name and position. Methods declared elsewhere
 * (a patch block, another file's type) stay top-level as `Type.method`.
 * Accessors synthesized for `pub` fields are not listed: the field is. */

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

static bool pos_le(IronLsp_Position a, IronLsp_Position b) {
    return a.line < b.line || (a.line == b.line && a.character <= b.character);
}

static bool range_contains(IronLsp_Range outer, IronLsp_Range inner) {
    return pos_le(outer.start, inner.start) && pos_le(inner.end, outer.end);
}

static bool span_eq(Iron_Span a, Iron_Span b) {
    return a.line == b.line && a.col == b.col &&
           a.end_line == b.end_line && a.end_col == b.end_col;
}

/* A declaration's span starts at its keyword (`func`); its range starts
 * at the modifiers written before it on the same line (`pub readonly`). */
static Iron_Span with_modifiers(Iron_Span sp, const IronLsp_Document *doc) {
    static const char *const mods[] = { "pub", "private", "readonly", "pure", "nocopy" };
    if (sp.line == 0 || sp.col == 0 || !doc->text) return sp;
    size_t ls = ilsp_byte_of_line(&doc->line_idx, sp.line - 1);
    size_t i = ls + sp.col - 1;
    if (ls > doc->text_len || i > doc->text_len) return sp;
    for (;;) {
        size_t j = i;
        while (j > ls && (doc->text[j - 1] == ' ' || doc->text[j - 1] == '\t')) j--;
        size_t k = j;
        while (k > ls && ((doc->text[k - 1] >= 'a' && doc->text[k - 1] <= 'z'))) k--;
        bool is_mod = false;
        for (size_t m = 0; m < sizeof(mods) / sizeof(mods[0]); m++) {
            if (strlen(mods[m]) == j - k && memcmp(doc->text + k, mods[m], j - k) == 0) {
                is_mod = true;
                break;
            }
        }
        if (!is_mod || k == j) break;
        i = k;
    }
    sp.col = (uint32_t)(i - ls) + 1;
    return sp;
}

/* Fill a symbol's range and selectionRange from its declaration. */
static void set_ranges(IronLsp_DocSymbol *s, const Iron_Node *decl,
                       IronLsp_Document *doc, IronLsp_PositionEncoding enc) {
    s->range = ilsp_span_to_lsp_range(with_modifiers(decl->span, doc), doc, enc);
    s->selection_range = s->range;
    Iron_Span ns = ilsp_nav_decl_name_span(decl, doc->text, doc->text_len);
    if (span_eq(ns, decl->span)) return;  /* name not found */
    IronLsp_Range sel = ilsp_span_to_lsp_range(ns, doc, enc);
    if (range_contains(s->range, sel)) s->selection_range = sel;
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
    set_ranges(&s, (Iron_Node *)f, doc, enc);
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
    set_ranges(&s, (Iron_Node *)v, doc, enc);
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
    set_ranges(&s, n, doc, enc);
    return s;
}

/* Build DocSymbol for top-level object with its fields; room is left for
 * `method_room` methods, attached by the caller. */
static IronLsp_DocSymbol build_object(Iron_ObjectDecl *o,
                                        int method_room,
                                        IronLsp_Document *doc,
                                        IronLsp_PositionEncoding enc,
                                        Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    s.name  = astrdup(arena, o->name);
    s.kind  = LSP_SYMKIND_CLASS;
    set_ranges(&s, (Iron_Node *)o, doc, enc);

    int cap = o->field_count + method_room;
    if (cap <= 0) return s;
    IronLsp_DocSymbol *kids = (IronLsp_DocSymbol *)iron_arena_alloc(
        arena, sizeof(IronLsp_DocSymbol) * (size_t)cap,
        _Alignof(IronLsp_DocSymbol));
    if (!kids) return s;
    size_t w = 0;
    for (int i = 0; i < o->field_count; i++) {
        Iron_Node *c = o->fields[i];
        if (!c || c->kind != IRON_NODE_FIELD) continue;
        kids[w++] = build_field((Iron_Field *)c, doc, enc, arena);
    }
    s.children = kids;
    s.child_count = w;
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
    set_ranges(&s, (Iron_Node *)i, doc, enc);

    if (i->method_count > 0) {
        IronLsp_DocSymbol *kids = (IronLsp_DocSymbol *)iron_arena_alloc(
            arena, sizeof(IronLsp_DocSymbol) * (size_t)i->method_count,
            _Alignof(IronLsp_DocSymbol));
        if (kids) {
            size_t w = 0;
            for (int j = 0; j < i->method_count; j++) {
                Iron_Node *c = i->method_sigs[j];
                if (!c || c->kind == IRON_NODE_ERROR) continue;
                kids[w++] = build_method_sig(c, doc, enc, arena);
            }
            s.children    = kids;
            s.child_count = w;
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
    set_ranges(&s, (Iron_Node *)e, doc, enc);

    if (e->variant_count > 0) {
        IronLsp_DocSymbol *kids = (IronLsp_DocSymbol *)iron_arena_alloc(
            arena, sizeof(IronLsp_DocSymbol) * (size_t)e->variant_count,
            _Alignof(IronLsp_DocSymbol));
        if (kids) {
            size_t w = 0;
            for (int j = 0; j < e->variant_count; j++) {
                Iron_Node *c = e->variants[j];
                if (!c || c->kind != IRON_NODE_ENUM_VARIANT) continue;
                kids[w++] = build_variant((Iron_EnumVariant *)c, doc, enc, arena);
            }
            s.children    = kids;
            s.child_count = w;
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
    set_ranges(&s, (Iron_Node *)f, doc, enc);
    return s;
}

/* A method; `qualified` labels it "Type.method" (a top-level symbol),
 * otherwise "method" (a child of its object). */
static IronLsp_DocSymbol build_method(Iron_MethodDecl *m,
                                        bool qualified,
                                        IronLsp_Document *doc,
                                        IronLsp_PositionEncoding enc,
                                        Iron_Arena *arena) {
    IronLsp_DocSymbol s;
    memset(&s, 0, sizeof(s));
    const char *type_name   = (qualified && m->type_name) ? m->type_name : "";
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
    set_ranges(&s, (Iron_Node *)m, doc, enc);
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
    set_ranges(&s, (Iron_Node *)v, doc, enc);
    return s;
}

static bool span_within(Iron_Span inner, Iron_Span outer) {
    uint32_t ie = inner.end_line ? inner.end_line : inner.line;
    uint32_t oe = outer.end_line ? outer.end_line : outer.line;
    if (inner.line < outer.line || (inner.line == outer.line && inner.col < outer.col))
        return false;
    if (ie > oe || (ie == oe && inner.end_col > outer.end_col)) return false;
    return true;
}

/* The object declared in this file whose block holds method `m`, or NULL. */
static Iron_ObjectDecl *owning_object(const Iron_Program *program,
                                      const Iron_MethodDecl *m) {
    if (!m->type_name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *o = (Iron_ObjectDecl *)d;
        if (o->name && strcmp(o->name, m->type_name) == 0 &&
            span_within(m->span, o->span)) return o;
    }
    return NULL;
}

static bool listed_method(const Iron_Node *d) {
    return d && d->kind == IRON_NODE_METHOD_DECL &&
           !((const Iron_MethodDecl *)d)->is_synth_accessor;
}

/* Children in source order (fields come first from build_object; a
 * method can sit between fields). */
static void sort_children(IronLsp_DocSymbol *s) {
    for (size_t i = 1; i < s->child_count; i++) {
        IronLsp_DocSymbol t = s->children[i];
        size_t j = i;
        while (j > 0 && !pos_le(s->children[j - 1].range.start, t.range.start)) {
            s->children[j] = s->children[j - 1];
            j--;
        }
        s->children[j] = t;
    }
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
    /* Which top-level symbol each object decl became, for its methods. */
    Iron_ObjectDecl **obj_of = (Iron_ObjectDecl **)iron_arena_alloc(
        &walk_arena, sizeof(Iron_ObjectDecl *) * (size_t)count,
        _Alignof(Iron_ObjectDecl *));
    if (!arr || !obj_of) goto done;

    size_t w = 0;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) continue;
        obj_of[w] = NULL;
        switch ((int)d->kind) {
            case IRON_NODE_IMPORT_DECL:
                arr[w++] = build_import((Iron_ImportDecl *)d, doc, enc, arena);
                break;
            case IRON_NODE_OBJECT_DECL: {
                Iron_ObjectDecl *o = (Iron_ObjectDecl *)d;
                int room = 0;
                for (int k = 0; k < program->decl_count; k++) {
                    Iron_Node *m = program->decls[k];
                    if (listed_method(m) &&
                        owning_object(program, (Iron_MethodDecl *)m) == o) room++;
                }
                obj_of[w] = o;
                arr[w++] = build_object(o, room, doc, enc, arena);
                break;
            }
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
                break;  /* below, once every object is built */
            case IRON_NODE_VAL_DECL:
                arr[w++] = build_val((Iron_ValDecl *)d, doc, enc, arena);
                break;
            default:
                /* Skip anything else (var decls at module level, stmt
                 * shapes, etc.). */
                break;
        }
    }

    /* Methods: under the object whose block declares them, else
     * top-level `Type.method` at their place in the file. */
    size_t top_n = w;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!listed_method(d)) continue;
        Iron_MethodDecl *m = (Iron_MethodDecl *)d;
        Iron_ObjectDecl *o = owning_object(program, m);
        IronLsp_DocSymbol *owner = NULL;
        for (size_t k = 0; o && k < top_n; k++) {
            if (obj_of[k] == o) { owner = &arr[k]; break; }
        }
        if (owner && owner->children) {
            owner->children[owner->child_count++] =
                build_method(m, false, doc, enc, arena);
        } else {
            arr[w++] = build_method(m, true, doc, enc, arena);
        }
    }
    for (size_t k = 0; k < top_n; k++) {
        if (obj_of[k]) sort_children(&arr[k]);
    }
    /* Top level in source order too (stray methods were appended). */
    IronLsp_DocSymbol top = { .children = arr, .child_count = w };
    sort_children(&top);

    *out_syms = arr;
    *out_n    = w;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
