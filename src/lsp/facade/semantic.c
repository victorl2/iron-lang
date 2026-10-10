/* semantic.c -- textDocument/semanticTokens/full and textDocument/inlayHint.
 *
 * Semantic tokens classify every name the analyzer resolved, so the editor
 * colors a type, a function, a parameter, a field or an enum variant as
 * what it is rather than by the TextMate grammar's guess: an identifier
 * through its symbol, a method or field name through its receiver, a type
 * annotation through its declaration, and each declaration's own name.
 * Names that come from the stdlib carry the defaultLibrary modifier and
 * `val` bindings the readonly modifier.
 *
 * Inlay hints show the inferred type after a `val` / `var` written
 * without one.
 *
 * Both analyze through ilsp_facade_compile_for_nav (the CORE-22 single
 * call site) and walk only the document's own declarations
 * (decls[0 .. decl_count)). */

#include "lsp/facade/semantic.h"

#include "lsp/facade/compile.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/server/cancel.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/utf.h"
#include "lsp/transport/json.h"
#include "lsp/transport/types.h"
#include "lsp/transport/writer.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"
#include "vendor/yyjson/yyjson.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ── Legend ───────────────────────────────────────────────────────── */

enum {
    TOK_TYPE, TOK_CLASS, TOK_ENUM, TOK_INTERFACE, TOK_ENUM_MEMBER,
    TOK_PARAMETER, TOK_VARIABLE, TOK_PROPERTY, TOK_FUNCTION, TOK_METHOD,
    TOK_COUNT
};
static const char *const k_token_types[TOK_COUNT] = {
    "type", "class", "enum", "interface", "enumMember",
    "parameter", "variable", "property", "function", "method",
};

enum { MOD_DECLARATION = 1u << 0, MOD_READONLY = 1u << 1, MOD_DEFAULT_LIBRARY = 1u << 2 };
static const char *const k_token_modifiers[] = {
    "declaration", "readonly", "defaultLibrary",
};

void ilsp_semantic_tokens_legend(yyjson_mut_doc *d, yyjson_mut_val *provider) {
    yyjson_mut_val *legend = yyjson_mut_obj(d);
    yyjson_mut_val *types = yyjson_mut_arr(d);
    for (int i = 0; i < TOK_COUNT; i++) yyjson_mut_arr_add_str(d, types, k_token_types[i]);
    yyjson_mut_val *mods = yyjson_mut_arr(d);
    for (size_t i = 0; i < sizeof(k_token_modifiers) / sizeof(k_token_modifiers[0]); i++) {
        yyjson_mut_arr_add_str(d, mods, k_token_modifiers[i]);
    }
    yyjson_mut_obj_add_val(d, legend, "tokenTypes", types);
    yyjson_mut_obj_add_val(d, legend, "tokenModifiers", mods);
    yyjson_mut_obj_add_val(d, provider, "legend", legend);
    yyjson_mut_obj_add_bool(d, provider, "full", true);
}

/* ── Source positions ─────────────────────────────────────────────── */

/* Byte offset of a 1-based (line, col) span position, or SIZE_MAX. */
static size_t offset_of(const IronLsp_Document *doc, uint32_t line, uint32_t col) {
    if (line == 0 || col == 0) return SIZE_MAX;
    size_t start = ilsp_byte_of_line(&doc->line_idx, line - 1);
    size_t off = start + (col - 1);
    return off <= doc->text_len ? off : SIZE_MAX;
}

static bool is_ident_byte(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* True when `name` is spelled at `off` as a whole word. */
static bool word_at(const IronLsp_Document *doc, size_t off, const char *name) {
    size_t n = strlen(name);
    if (n == 0 || off == SIZE_MAX || off + n > doc->text_len) return false;
    if (memcmp(doc->text + off, name, n) != 0) return false;
    if (off > 0 && is_ident_byte(doc->text[off - 1])) return false;
    if (off + n < doc->text_len && is_ident_byte(doc->text[off + n])) return false;
    return true;
}

/* First whole-word `name` from `off` to the end of that line. */
static size_t find_word_on_line(const IronLsp_Document *doc, size_t off, const char *name) {
    if (off == SIZE_MAX || !name || !*name) return SIZE_MAX;
    for (size_t i = off; i < doc->text_len && doc->text[i] != '\n'; i++) {
        if (word_at(doc, i, name)) return i;
    }
    return SIZE_MAX;
}

/* The member name after `object` in `object.name` (whitespace and line
 * breaks allowed around the dot), or SIZE_MAX. */
static size_t member_name_after(const IronLsp_Document *doc, const Iron_Node *object,
                                const char *name) {
    if (!object || !name) return SIZE_MAX;
    size_t i = offset_of(doc, object->span.end_line, object->span.end_col);
    if (i == SIZE_MAX) return SIZE_MAX;
    i++;  /* end_col is the object's last byte */
    while (i < doc->text_len && isspace((unsigned char)doc->text[i])) i++;
    if (i >= doc->text_len || doc->text[i] != '.') return SIZE_MAX;
    i++;
    while (i < doc->text_len && isspace((unsigned char)doc->text[i])) i++;
    return word_at(doc, i, name) ? i : SIZE_MAX;
}

/* Fallback when the receiver's end position is off (some literals'
 * spans stop early): scan from its start, outside brackets and strings,
 * for the first `.name`. */
static size_t member_name_scan(const IronLsp_Document *doc, const Iron_Node *object,
                               const char *name) {
    if (!object || !name) return SIZE_MAX;
    size_t i = offset_of(doc, object->span.line, object->span.col);
    if (i == SIZE_MAX) return SIZE_MAX;
    size_t limit = i + 4096 < doc->text_len ? i + 4096 : doc->text_len;
    int depth = 0;
    for (; i < limit; i++) {
        char ch = doc->text[i];
        if (ch == '"') {
            for (i++; i < limit && doc->text[i] != '"'; i++) {
                if (doc->text[i] == '\\') i++;
            }
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') depth++;
        else if (ch == ')' || ch == ']' || ch == '}') { if (--depth < 0) return SIZE_MAX; }
        else if (ch == '.' && depth == 0) {
            size_t j = i + 1;
            while (j < limit && isspace((unsigned char)doc->text[j])) j++;
            if (word_at(doc, j, name)) return j;
        }
    }
    return SIZE_MAX;
}

static size_t member_name_at(const IronLsp_Document *doc, const Iron_Node *object,
                             const char *name) {
    size_t off = member_name_after(doc, object, name);
    return off != SIZE_MAX ? off : member_name_scan(doc, object, name);
}

/* ── Token collection ─────────────────────────────────────────────── */

typedef struct {
    size_t   off;   /* byte offset in the document */
    uint32_t len;   /* bytes */
    uint32_t type;
    uint32_t mods;
} SemTok;

typedef struct {
    const IronLsp_Document *doc;
    const Iron_Program     *program;
    Iron_Arena             *arena;
    SemTok                 *toks;   /* stb_ds */
} TokCtx;

static void add_tok(TokCtx *c, size_t off, const char *name, uint32_t type, uint32_t mods) {
    if (off == SIZE_MAX || !name) return;
    SemTok t = { off, (uint32_t)strlen(name), type, mods };
    arrput(c->toks, t);
}

static bool from_prelude(const TokCtx *c, const Iron_Node *decl) {
    return decl && decl->span.filename && c->doc->uri &&
           strcmp(decl->span.filename, c->doc->uri) != 0;
}

/* Token type for a declaration node. */
static int decl_token(const Iron_Node *decl, uint32_t *mods) {
    switch ((int)decl->kind) {
        case IRON_NODE_OBJECT_DECL:    return TOK_CLASS;
        case IRON_NODE_ENUM_DECL:      return TOK_ENUM;
        case IRON_NODE_INTERFACE_DECL: return TOK_INTERFACE;
        case IRON_NODE_ENUM_VARIANT:   return TOK_ENUM_MEMBER;
        case IRON_NODE_FIELD:          return TOK_PROPERTY;
        case IRON_NODE_PARAM:          return TOK_PARAMETER;
        case IRON_NODE_FUNC_DECL:      return TOK_FUNCTION;
        case IRON_NODE_METHOD_DECL:    return TOK_METHOD;
        case IRON_NODE_VAL_DECL:       *mods |= MOD_READONLY; return TOK_VARIABLE;
        case IRON_NODE_VAR_DECL:       return TOK_VARIABLE;
        default:                       return -1;
    }
}

static int symbol_token(const Iron_Symbol *sym, uint32_t *mods) {
    if (sym->decl_node) {
        int t = decl_token(sym->decl_node, mods);
        if (t >= 0) return t;
    }
    switch ((int)sym->sym_kind) {
        case IRON_SYM_VARIABLE:     return TOK_VARIABLE;
        case IRON_SYM_FUNCTION:     return TOK_FUNCTION;
        case IRON_SYM_METHOD:       return TOK_METHOD;
        case IRON_SYM_TYPE:         return TOK_CLASS;
        case IRON_SYM_ENUM:         return TOK_ENUM;
        case IRON_SYM_ENUM_VARIANT: return TOK_ENUM_MEMBER;
        case IRON_SYM_INTERFACE:    return TOK_INTERFACE;
        case IRON_SYM_PARAM:        return TOK_PARAMETER;
        case IRON_SYM_FIELD:        return TOK_PROPERTY;
        default:                    return -1;
    }
}

/* The declaration's own name, marked as a declaration. */
static void add_decl_name(TokCtx *c, Iron_Node *n) {
    const char *name = NULL;
    switch ((int)n->kind) {
        case IRON_NODE_FUNC_DECL:      name = ((Iron_FuncDecl *)n)->name; break;
        case IRON_NODE_METHOD_DECL:    name = ((Iron_MethodDecl *)n)->method_name; break;
        case IRON_NODE_OBJECT_DECL:
            if (((Iron_ObjectDecl *)n)->is_patch) return;
            name = ((Iron_ObjectDecl *)n)->name;
            break;
        case IRON_NODE_ENUM_DECL:      name = ((Iron_EnumDecl *)n)->name; break;
        case IRON_NODE_INTERFACE_DECL: name = ((Iron_InterfaceDecl *)n)->name; break;
        case IRON_NODE_ENUM_VARIANT:   name = ((Iron_EnumVariant *)n)->name; break;
        case IRON_NODE_FIELD:          name = ((Iron_Field *)n)->name; break;
        case IRON_NODE_PARAM:          name = ((Iron_Param *)n)->name; break;
        case IRON_NODE_VAL_DECL:       name = ((Iron_ValDecl *)n)->name; break;
        case IRON_NODE_VAR_DECL:       name = ((Iron_VarDecl *)n)->name; break;
        default: return;
    }
    if (!name || strcmp(name, "self") == 0) return;
    uint32_t mods = MOD_DECLARATION;
    int type = decl_token(n, &mods);
    if (type < 0) return;
    if (n->kind == IRON_NODE_METHOD_DECL && ((Iron_MethodDecl *)n)->is_init) return;
    size_t off = find_word_on_line(c->doc, offset_of(c->doc, n->span.line, n->span.col), name);
    add_tok(c, off, name, (uint32_t)type, mods);
}

static bool tok_visit(Iron_Visitor *v, Iron_Node *n) {
    TokCtx *c = (TokCtx *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->span.line == 0) return true;
    if (n->span.filename && c->doc->uri && strcmp(n->span.filename, c->doc->uri) != 0) {
        return true;  /* spliced in from elsewhere: no position here */
    }

    switch ((int)n->kind) {
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)n;
            if (!id->name || !id->resolved_sym || strcmp(id->name, "self") == 0) break;
            uint32_t mods = 0;
            int type = symbol_token(id->resolved_sym, &mods);
            if (type < 0) break;
            if (!id->resolved_sym->decl_node ||  /* a builtin: println, len... */
                from_prelude(c, id->resolved_sym->decl_node)) {
                mods |= MOD_DEFAULT_LIBRARY;
            }
            size_t off = offset_of(c->doc, n->span.line, n->span.col);
            if (word_at(c->doc, off, id->name)) add_tok(c, off, id->name, (uint32_t)type, mods);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            if (mc->is_ctor_desugar) break;
            Iron_Node *decl = ilsp_nav_member_decl(c->program, n, c->arena);
            uint32_t mods = (!decl || from_prelude(c, decl)) ? MOD_DEFAULT_LIBRARY : 0;
            add_tok(c, member_name_at(c->doc, mc->object, mc->method), mc->method,
                    TOK_METHOD, mods);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
            Iron_Node *decl = ilsp_nav_member_decl(c->program, n, c->arena);
            uint32_t mods = 0;
            uint32_t type = TOK_PROPERTY;
            if (decl && decl->kind == IRON_NODE_ENUM_VARIANT) type = TOK_ENUM_MEMBER;
            if (decl && decl->kind == IRON_NODE_FIELD && !((Iron_Field *)decl)->is_var) {
                mods |= MOD_READONLY;
            }
            if (decl && from_prelude(c, decl)) mods |= MOD_DEFAULT_LIBRARY;
            add_tok(c, member_name_at(c->doc, fa->object, fa->field), fa->field, type, mods);
            break;
        }
        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)n;
            if (!ec->enum_name || !ec->variant_name) break;
            Iron_Node *decl = ilsp_nav_member_decl(c->program, n, c->arena);
            uint32_t mods = (decl && from_prelude(c, decl)) ? MOD_DEFAULT_LIBRARY : 0;
            size_t off = offset_of(c->doc, n->span.line, n->span.col);
            if (!word_at(c->doc, off, ec->enum_name)) break;  /* `.Circle` shorthand */
            add_tok(c, off, ec->enum_name, TOK_ENUM, mods);
            size_t v = off + strlen(ec->enum_name);
            while (v < c->doc->text_len && isspace((unsigned char)c->doc->text[v])) v++;
            if (v < c->doc->text_len && c->doc->text[v] == '.') {
                v++;
                while (v < c->doc->text_len && isspace((unsigned char)c->doc->text[v])) v++;
                if (word_at(c->doc, v, ec->variant_name)) {
                    add_tok(c, v, ec->variant_name, TOK_ENUM_MEMBER, mods);
                }
            }
            break;
        }
        case IRON_NODE_TYPE_ANNOTATION: {
            Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)n;
            if (!ta->name || ta->is_func || ta->is_tuple || ta->is_self_type) break;
            size_t off = offset_of(c->doc, n->span.line, n->span.col);
            if (!word_at(c->doc, off, ta->name)) break;
            Iron_Node *decl = ilsp_nav_member_decl(c->program, n, c->arena);
            uint32_t mods = 0;
            uint32_t type = TOK_TYPE;
            if (decl) {
                int t = decl_token(decl, &mods);
                if (t >= 0) type = (uint32_t)t;
                if (from_prelude(c, decl)) mods |= MOD_DEFAULT_LIBRARY;
            } else {
                mods |= MOD_DEFAULT_LIBRARY;  /* Int, String, a generic T... */
            }
            add_tok(c, off, ta->name, type, mods);
            break;
        }
        default:
            add_decl_name(c, n);
            break;
    }
    return true;
}

static int cmp_tok(const void *a, const void *b) {
    const SemTok *x = (const SemTok *)a, *y = (const SemTok *)b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return 0;
}

/* Position of a byte offset in the client's encoding. */
static void lsp_pos(const IronLsp_Document *doc, size_t off, IronLsp_PositionEncoding enc,
                    uint32_t *line, uint32_t *character) {
    uint32_t l = ilsp_line_of_byte(&doc->line_idx, off);
    size_t start = ilsp_byte_of_line(&doc->line_idx, l);
    size_t end = start;
    while (end < doc->text_len && doc->text[end] != '\n') end++;
    const char *lt = doc->text + start;
    size_t in_line = off - start;
    *line = l;
    *character = (enc == ILSP_ENC_UTF16)
        ? ilsp_utf8_byte_to_utf16_column(lt, end - start, in_line)
        : ilsp_utf8_byte_to_utf8_column(lt, end - start, in_line);
}

uint32_t *ilsp_facade_semantic_tokens(IronLsp_Server *server, IronLsp_Document *doc,
                                      _Atomic bool *cancel, Iron_Arena *arena,
                                      size_t *out_n) {
    *out_n = 0;
    if (!server || !doc || !doc->text) return NULL;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags      = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version, .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(doc, &req, &walk_arena, &diags);
    uint32_t *data = NULL;

    if (program && !(cancel && atomic_load(cancel))) {
        TokCtx c = { doc, program, &walk_arena, NULL };
        Iron_Visitor v = { .ctx = &c, .visit_node = tok_visit, .post_visit = NULL };
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            /* Instances of generics repeat their template's spans. */
            if (d) iron_ast_walk(d, &v);
        }
        size_t n = (size_t)arrlen(c.toks);
        if (n > 0) qsort(c.toks, n, sizeof(SemTok), cmp_tok);

        data = (uint32_t *)iron_arena_alloc(arena, sizeof(uint32_t) * 5 * (n ? n : 1),
                                            _Alignof(uint32_t));
        IronLsp_PositionEncoding enc = server->position_encoding;
        uint32_t prev_line = 0, prev_char = 0;
        size_t prev_end = 0, k = 0;
        for (size_t i = 0; data && i < n; i++) {
            const SemTok *t = &c.toks[i];
            if (i > 0 && t->off < prev_end) continue;  /* duplicate or overlap */
            uint32_t line, ch, end_line, end_ch;
            lsp_pos(doc, t->off, enc, &line, &ch);
            lsp_pos(doc, t->off + t->len, enc, &end_line, &end_ch);
            if (end_line != line) continue;
            data[k * 5 + 0] = line - prev_line;
            data[k * 5 + 1] = (line == prev_line) ? ch - prev_char : ch;
            data[k * 5 + 2] = end_ch - ch;
            data[k * 5 + 3] = t->type;
            data[k * 5 + 4] = t->mods;
            prev_line = line;
            prev_char = ch;
            prev_end = t->off + t->len;
            k++;
        }
        *out_n = k * 5;
        arrfree(c.toks);
    }

    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
    return data;
}

/* ── Inlay hints ──────────────────────────────────────────────────── */

typedef struct {
    const IronLsp_Document *doc;
    const Iron_Program     *program;
    Iron_Arena             *walk_arena;
    Iron_Arena             *out_arena;
    uint32_t                first_line, last_line;  /* 1-based, inclusive */
    IronLsp_InlayHint      *hints;                  /* stb_ds */
    bool                    param_names;            /* settings */
    bool                    binding_types;
} HintCtx;

static bool is_literal(const Iron_Node *n) {
    return n->kind == IRON_NODE_INT_LIT || n->kind == IRON_NODE_FLOAT_LIT ||
           n->kind == IRON_NODE_STRING_LIT || n->kind == IRON_NODE_BOOL_LIT ||
           n->kind == IRON_NODE_NULL_LIT;
}

/* `area(w: 2, h: 3)`: each argument's parameter name, except where the
 * argument already says it (`area(w, h)`, `area(p.w, ...)`) and for a
 * single parameter unless the argument is a literal. */
static void param_hints_named(HintCtx *c, const char *const *names, int named,
                              Iron_Node **args, int arg_count) {
    for (int i = 0; i < arg_count && i < named; i++) {
        Iron_Node *a = args[i];
        const char *name = names[i];
        if (!a || !name || !*name || a->span.line == 0) continue;
        if (a->span.line < c->first_line || a->span.line > c->last_line) continue;
        if (named == 1 && !is_literal(a)) continue;
        if (a->kind == IRON_NODE_IDENT && ((Iron_Ident *)a)->name &&
            strcmp(((Iron_Ident *)a)->name, name) == 0) continue;
        if (a->kind == IRON_NODE_FIELD_ACCESS && ((Iron_FieldAccess *)a)->field &&
            strcmp(((Iron_FieldAccess *)a)->field, name) == 0) continue;
        size_t off = offset_of(c->doc, a->span.line, a->span.col);
        if (off == SIZE_MAX) continue;
        size_t len = strlen(name) + 2;
        char *label = (char *)iron_arena_alloc(c->out_arena, len, 1);
        if (!label) continue;
        snprintf(label, len, "%s:", name);
        IronLsp_InlayHint h = { .off = off, .label = label, .kind = 2 };
        arrput(c->hints, h);
    }
}

static void param_hints(HintCtx *c, Iron_Node **params, int param_count,
                        Iron_Node **args, int arg_count) {
    int first = 0;
    if (param_count > 0 && params[0] && ((Iron_Param *)params[0])->name &&
        strcmp(((Iron_Param *)params[0])->name, "self") == 0) first = 1;
    int named = param_count - first;
    if (named <= 0) return;
    const char **names = (const char **)iron_arena_alloc(
        c->walk_arena, sizeof(const char *) * (size_t)named, _Alignof(const char *));
    if (!names) return;
    for (int i = 0; i < named; i++) {
        Iron_Param *pm = (Iron_Param *)params[first + i];
        names[i] = (pm && pm->kind == IRON_NODE_PARAM) ? pm->name : NULL;
    }
    param_hints_named(c, names, named, args, arg_count);
}

/* `P(x: 1, y: 2)`: a construction names the parameters of the object's
 * anonymous `init` taking that many arguments or, when it has no init,
 * its fields in order. */
static void construct_hints(HintCtx *c, Iron_ObjectDecl *od, Iron_Node **args,
                            int arg_count) {
    const Iron_Program *p = c->program;
    bool has_init = false;
    for (int i = 0; i < p->decl_count + p->prelude_decl_count; i++) {
        Iron_Node *d = p->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->is_init || md->init_name || !md->type_name || !od->name ||
            strcmp(md->type_name, od->name) != 0) continue;
        has_init = true;
        int n = md->param_count;
        if (n > 0 && md->params[0] && ((Iron_Param *)md->params[0])->name &&
            strcmp(((Iron_Param *)md->params[0])->name, "self") == 0) n--;
        if (n == arg_count) {
            param_hints(c, md->params, md->param_count, args, arg_count);
            return;
        }
    }
    if (has_init || od->field_count <= 0) return;
    const char **names = (const char **)iron_arena_alloc(
        c->walk_arena, sizeof(const char *) * (size_t)od->field_count, _Alignof(const char *));
    if (!names) return;
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        names[i] = (f && f->kind == IRON_NODE_FIELD) ? f->name : NULL;
    }
    param_hints_named(c, names, od->field_count, args, arg_count);
}

static bool hint_visit(Iron_Visitor *v, Iron_Node *n) {
    HintCtx *c = (HintCtx *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->span.filename && c->doc->uri && strcmp(n->span.filename, c->doc->uri) != 0) {
        return true;
    }
    if (n->kind == IRON_NODE_CALL) {
        Iron_CallExpr *call = (Iron_CallExpr *)n;
        if (!c->param_names) return true;
        if (call->callee && call->callee->kind == IRON_NODE_IDENT) {
            const Iron_Symbol *sym = ((Iron_Ident *)call->callee)->resolved_sym;
            Iron_Node *d = sym ? sym->decl_node : NULL;
            if (d && d->kind == IRON_NODE_FUNC_DECL) {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
                param_hints(c, fd->params, fd->param_count, call->args, call->arg_count);
            } else if (d && d->kind == IRON_NODE_OBJECT_DECL) {
                construct_hints(c, (Iron_ObjectDecl *)d, call->args, call->arg_count);
            }
        }
        return true;
    }
    if (n->kind == IRON_NODE_METHOD_CALL) {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
        if (!c->param_names) return true;
        Iron_Node *d = ilsp_nav_member_decl(c->program, n, c->walk_arena);
        if (d && d->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            param_hints(c, md->params, md->param_count, mc->args, mc->arg_count);
        } else if (d && d->kind == IRON_NODE_FUNC_DECL) {
            /* An interface's signature. */
            Iron_FuncDecl *sig = (Iron_FuncDecl *)d;
            param_hints(c, sig->params, sig->param_count, mc->args, mc->arg_count);
        }
        return true;
    }
    if (!c->binding_types) return true;
    const char *name = NULL;
    const Iron_Type *t = NULL;
    if (n->kind == IRON_NODE_VAL_DECL) {
        Iron_ValDecl *vd = (Iron_ValDecl *)n;
        if (vd->type_ann || vd->binding_count > 0) return true;
        name = vd->name;
        t = vd->declared_type;
    } else if (n->kind == IRON_NODE_VAR_DECL) {
        Iron_VarDecl *vd = (Iron_VarDecl *)n;
        if (vd->type_ann) return true;
        name = vd->name;
        t = vd->declared_type;
    } else {
        return true;
    }
    if (!name || !t || t->kind == IRON_TYPE_ERROR) return true;
    if (n->span.line < c->first_line || n->span.line > c->last_line) return true;
    size_t off = find_word_on_line(c->doc, offset_of(c->doc, n->span.line, n->span.col), name);
    if (off == SIZE_MAX) return true;
    const char *ts = iron_type_to_string(t, c->out_arena);
    if (!ts) return true;
    size_t len = strlen(ts) + 3;
    char *label = (char *)iron_arena_alloc(c->out_arena, len, 1);
    if (!label) return true;
    snprintf(label, len, ": %s", ts);
    IronLsp_InlayHint h = { .off = off + strlen(name), .label = label, .kind = 1 };
    arrput(c->hints, h);
    return true;
}

static int cmp_hint(const void *a, const void *b) {
    const IronLsp_InlayHint *x = (const IronLsp_InlayHint *)a;
    const IronLsp_InlayHint *y = (const IronLsp_InlayHint *)b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return 0;
}

IronLsp_InlayHint *ilsp_facade_inlay_hints(IronLsp_Server *server, IronLsp_Document *doc,
                                           uint32_t first_line0, uint32_t last_line0,
                                           _Atomic bool *cancel, Iron_Arena *arena,
                                           size_t *out_n) {
    *out_n = 0;
    if (!server || !doc || !doc->text) return NULL;
    bool param_names = !atomic_load(&server->inlay_hide_parameter_names);
    bool binding_types = !atomic_load(&server->inlay_hide_binding_types);
    if (!param_names && !binding_types) return NULL;  /* both turned off */

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags      = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version, .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(doc, &req, &walk_arena, &diags);
    IronLsp_InlayHint *out = NULL;

    if (program && !(cancel && atomic_load(cancel))) {
        HintCtx c = { doc, program, &walk_arena, arena, first_line0 + 1, last_line0 + 1, NULL,
                      param_names, binding_types };
        Iron_Visitor v = { .ctx = &c, .visit_node = hint_visit, .post_visit = NULL };
        for (int i = 0; i < program->decl_count; i++) {
            if (program->decls[i]) iron_ast_walk(program->decls[i], &v);
        }
        size_t n = (size_t)arrlen(c.hints);
        if (n > 0) qsort(c.hints, n, sizeof(IronLsp_InlayHint), cmp_hint);
        out = (IronLsp_InlayHint *)iron_arena_alloc(
            arena, sizeof(IronLsp_InlayHint) * (n ? n : 1), _Alignof(IronLsp_InlayHint));
        size_t k = 0;
        for (size_t i = 0; out && i < n; i++) {
            if (k > 0 && out[k - 1].off == c.hints[i].off) continue;  /* generic instances */
            out[k] = c.hints[i];
            lsp_pos(doc, out[k].off, server->position_encoding, &out[k].line, &out[k].character);
            k++;
        }
        *out_n = k;
        arrfree(c.hints);
    }

    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
    return out;
}

/* ── Handlers ─────────────────────────────────────────────────────── */

static void respond(IronLsp_Server *s, Iron_Arena *arena, yyjson_val *id_v,
                    yyjson_mut_doc *rd, yyjson_mut_val *result) {
    ilsp_nav_wrap_response(rd, id_v, result);
    size_t len = 0;
    char *body = ilsp_json_write_mut(rd, arena, &len);
    if (!body || len == 0 || !s->writer) return;
    char *heap = (char *)malloc(len);
    if (!heap) return;
    memcpy(heap, body, len);
    ilsp_writer_enqueue(s->writer, ILSP_PRIO_RESPONSE, heap, len);
}

static IronLsp_Document *find_doc(IronLsp_Server *s, const char *uri) {
    if (!s->documents || !uri) return NULL;
    ptrdiff_t idx = shgeti(s->documents, uri);
    return idx < 0 ? NULL : s->documents[idx].value;
}

static _Atomic bool *cancel_flag_for(IronLsp_Server *s, yyjson_val *id_v) {
    if (!s->cancels) return NULL;
    char *key = ilsp_nav_stringify_id(id_v);
    if (!key) return NULL;
    _Atomic bool *flag = ilsp_cancel_register(s->cancels, key);
    free(key);
    return flag;
}

void ilsp_handle_text_document_semantic_tokens_full(IronLsp_Server    *s,
                                                    struct yyjson_doc *doc,
                                                    Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    Iron_Arena      body_arena = iron_arena_create(16 * 1024);
    yyjson_alc      alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd         = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    IronLsp_Document *d = find_doc(s, ilsp_nav_parse_uri(params));
    Iron_Arena work_arena = iron_arena_create(16 * 1024);
    size_t n = 0;
    uint32_t *data = NULL;
    if (d) {
        _Atomic bool *cancel = cancel_flag_for(s, id_v);
        data = ilsp_facade_semantic_tokens(s, d, cancel, &work_arena, &n);
        if (cancel && atomic_load(cancel)) goto out;
    }
    yyjson_mut_val *result = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; data && i < n; i++) yyjson_mut_arr_add_uint(rd, arr, data[i]);
    yyjson_mut_obj_add_val(rd, result, "data", arr);
    respond(s, &body_arena, id_v, rd, result);

out:
    yyjson_mut_doc_free(rd);
    iron_arena_free(&work_arena);
    iron_arena_free(&body_arena);
}

void ilsp_handle_text_document_inlay_hint(IronLsp_Server    *s,
                                          struct yyjson_doc *doc,
                                          Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    uint32_t first = 0, last = UINT32_MAX - 1;
    yyjson_val *range = yyjson_obj_get(params, "range");
    if (range) {
        yyjson_val *st = yyjson_obj_get(yyjson_obj_get(range, "start"), "line");
        yyjson_val *en = yyjson_obj_get(yyjson_obj_get(range, "end"), "line");
        if (st && yyjson_is_uint(st)) first = (uint32_t)yyjson_get_uint(st);
        if (en && yyjson_is_uint(en)) last = (uint32_t)yyjson_get_uint(en);
    }

    Iron_Arena      body_arena = iron_arena_create(8 * 1024);
    yyjson_alc      alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd         = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    IronLsp_Document *d = find_doc(s, ilsp_nav_parse_uri(params));
    Iron_Arena work_arena = iron_arena_create(16 * 1024);
    size_t n = 0;
    IronLsp_InlayHint *hints = NULL;
    if (d) {
        _Atomic bool *cancel = cancel_flag_for(s, id_v);
        hints = ilsp_facade_inlay_hints(s, d, first, last, cancel, &work_arena, &n);
        if (cancel && atomic_load(cancel)) goto out;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; hints && i < n; i++) {
        yyjson_mut_val *h = yyjson_mut_obj(rd);
        yyjson_mut_val *pos = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, pos, "line", hints[i].line);
        yyjson_mut_obj_add_uint(rd, pos, "character", hints[i].character);
        yyjson_mut_obj_add_val(rd, h, "position", pos);
        yyjson_mut_obj_add_strcpy(rd, h, "label", hints[i].label);
        yyjson_mut_obj_add_uint(rd, h, "kind", (uint64_t)hints[i].kind);
        if (hints[i].kind == 2) yyjson_mut_obj_add_bool(rd, h, "paddingRight", true);
        yyjson_mut_arr_append(arr, h);
    }
    respond(s, &body_arena, id_v, rd, arr);

out:
    yyjson_mut_doc_free(rd);
    iron_arena_free(&work_arena);
    iron_arena_free(&body_arena);
}

/* ── Inlay hint settings ──────────────────────────────────────────── */

static bool apply_flag(yyjson_val *obj, const char *key, _Atomic bool *hide) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v || !yyjson_is_bool(v)) return false;
    bool new_hide = !yyjson_get_bool(v);
    return atomic_exchange(hide, new_hide) != new_hide;
}

bool ilsp_inlay_apply_settings(IronLsp_Server *s, yyjson_val *inlay_hints) {
    if (!s || !inlay_hints || !yyjson_is_obj(inlay_hints)) return false;
    bool changed = apply_flag(inlay_hints, "parameterNames", &s->inlay_hide_parameter_names);
    changed |= apply_flag(inlay_hints, "bindingTypes", &s->inlay_hide_binding_types);
    return changed;
}

/* workspace/inlayHint/refresh is a server-to-client request with no
 * params; the client's null response is not correlated. */
static void send_inlay_refresh(IronLsp_Server *s) {
    if (!s->writer) return;
    Iron_Arena arena = iron_arena_create(1024);
    yyjson_alc alc = ilsp_json_alc(&arena);
    yyjson_mut_doc *d = yyjson_mut_doc_new(&alc);
    if (d) {
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_doc_set_root(d, root);
        yyjson_mut_obj_add_strcpy(d, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_uint(d, root, "id", atomic_fetch_add(&s->next_request_id, 1));
        yyjson_mut_obj_add_strcpy(d, root, "method", "workspace/inlayHint/refresh");
        size_t len = 0;
        char *body = ilsp_json_write_mut(d, &arena, &len);
        char *heap = (body && len) ? (char *)malloc(len) : NULL;
        if (heap) {
            memcpy(heap, body, len);
            ilsp_writer_enqueue(s->writer, ILSP_PRIO_NOTIFICATION, heap, len);
        }
        yyjson_mut_doc_free(d);
    }
    iron_arena_free(&arena);
}

void ilsp_handle_workspace_did_change_configuration(IronLsp_Server    *s,
                                                    struct yyjson_doc *doc,
                                                    Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *params   = yyjson_obj_get(yyjson_doc_get_root(doc), "params");
    yyjson_val *settings = params ? yyjson_obj_get(params, "settings") : NULL;
    yyjson_val *iron     = settings ? yyjson_obj_get(settings, "iron") : NULL;
    yyjson_val *hints    = iron ? yyjson_obj_get(iron, "inlayHints") : NULL;
    if (ilsp_inlay_apply_settings(s, hints) && s->client_supports_inlay_refresh) {
        send_inlay_refresh(s);
    }
}
