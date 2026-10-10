/* Phase 3 Plan 03 Task 02 -- shared helpers for NAV facade TUs.
 *
 * All functions are pure and arena-aware. The per-request facade
 * arena owns every string returned through these helpers. */

#include "lsp/facade/nav/nav_common.h"

#include "lsp/store/utf.h"
#include "lsp/transport/json.h"
#include "parser/ast.h"
#include "analyzer/types.h"
#include "analyzer/scope.h"
#include "hir/stdlib_origin.h"
#include "vendor/stb_ds.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── JSON parameter parsing ──────────────────────────────────────── */

bool ilsp_nav_parse_position(yyjson_val *params, IronLsp_Position *out) {
    if (!params || !out) return false;
    yyjson_val *p = yyjson_obj_get(params, "position");
    if (!p || !yyjson_is_obj(p)) return false;
    yyjson_val *line_v = yyjson_obj_get(p, "line");
    yyjson_val *char_v = yyjson_obj_get(p, "character");
    if (!line_v || !char_v) return false;
    out->line      = (uint32_t)yyjson_get_uint(line_v);
    out->character = (uint32_t)yyjson_get_uint(char_v);
    return true;
}

const char *ilsp_nav_parse_uri(yyjson_val *params) {
    if (!params) return NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (!td) return NULL;
    return yyjson_get_str(yyjson_obj_get(td, "uri"));
}

char *ilsp_nav_stringify_id(yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return NULL;
    if (yyjson_is_int(id) || yyjson_is_sint(id)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld",
                  (long long)yyjson_get_sint(id));
        return strdup(buf);
    }
    if (yyjson_is_uint(id)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llu",
                  (unsigned long long)yyjson_get_uint(id));
        return strdup(buf);
    }
    if (yyjson_is_str(id)) return strdup(yyjson_get_str(id));
    return NULL;
}

/* ── Span -> Range via line index ────────────────────────────────── */

static void attach_range_local(yyjson_mut_doc *d, yyjson_mut_val *parent,
                                const char *key, IronLsp_Range r) {
    yyjson_mut_val *range = yyjson_mut_obj(d);
    yyjson_mut_val *start = yyjson_mut_obj(d);
    yyjson_mut_val *end   = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, start, "line",      r.start.line);
    yyjson_mut_obj_add_uint(d, start, "character", r.start.character);
    yyjson_mut_obj_add_uint(d, end,   "line",      r.end.line);
    yyjson_mut_obj_add_uint(d, end,   "character", r.end.character);
    yyjson_mut_obj_add_val (d, range, "start", start);
    yyjson_mut_obj_add_val (d, range, "end",   end);
    yyjson_mut_obj_add_val (d, parent, key, range);
}

/* Given a line number (1-based Iron) and a column (1-based byte offset
 * within that line), convert to LSP (0-based line, encoding-aware
 * character). Mirrors ilsp_span_to_lsp_range's math but operates on an
 * external buffer. */
static IronLsp_Position endpoint_to_lsp(const char *text, size_t text_len,
                                         const IronLsp_LineIndex *idx,
                                         uint32_t iron_line, uint32_t iron_col,
                                         IronLsp_PositionEncoding enc) {
    IronLsp_Position p = { .line = 0, .character = 0 };
    if (iron_line == 0) return p;
    /* Iron is 1-based; LSP is 0-based. */
    uint32_t lsp_line = iron_line - 1;
    p.line = lsp_line;

    /* Resolve line's byte offset. */
    size_t line_start = 0;
    if (idx) {
        line_start = ilsp_byte_of_line(idx, lsp_line);
    }
    if (line_start > text_len) line_start = text_len;

    /* Walk until '\n' to find line length. */
    size_t line_end = line_start;
    while (line_end < text_len && text[line_end] != '\n') line_end++;
    size_t line_len = line_end - line_start;

    /* iron_col is 1-based byte offset into the line. */
    size_t byte_in_line = (iron_col > 0) ? (iron_col - 1) : 0;
    if (byte_in_line > line_len) byte_in_line = line_len;

    const char *line_text = text + line_start;
    uint32_t character;
    if (enc == ILSP_ENC_UTF16) {
        character = (uint32_t)ilsp_utf8_byte_to_utf16_column(
            line_text, line_len, byte_in_line);
    } else {
        character = (uint32_t)ilsp_utf8_byte_to_utf8_column(
            line_text, line_len, byte_in_line);
    }
    p.character = character;
    return p;
}

IronLsp_Range ilsp_nav_span_to_range_via_lineidx(
    Iron_Span                       span,
    const char                     *text,
    size_t                          text_len,
    const IronLsp_LineIndex        *idx,
    IronLsp_PositionEncoding        enc) {
    IronLsp_Range r = { {0, 0}, {0, 0} };
    if (!text || text_len == 0) return r;
    /* Mirror facade/span.c: convert each endpoint identically (end_col
     * is treated as 1-based byte-offset, matching the Iron_Span
     * contract used throughout the compiler). */
    r.start = endpoint_to_lsp(text, text_len, idx,
                               span.line, span.col, enc);
    r.end = endpoint_to_lsp(text, text_len, idx,
                             span.end_line, span.end_col, enc);
    /* Unset end_line collapses range to zero-width at start. */
    if (span.end_line == 0 && span.line != 0) {
        r.end = r.start;
    }
    return r;
}

IronLsp_Range ilsp_nav_entry_span_to_range(
    const IronLsp_IndexEntry       *entry,
    Iron_Span                       span,
    IronLsp_PositionEncoding        enc) {
    IronLsp_Range r = { {0, 0}, {0, 0} };
    if (!entry || !entry->source_bytes) return r;
    return ilsp_nav_span_to_range_via_lineidx(
        span, entry->source_bytes, entry->source_len, &entry->line_idx, enc);
}

/* ── URI / path helpers ──────────────────────────────────────────── */

const char *ilsp_nav_path_to_uri(const char *canonical_path, Iron_Arena *arena) {
    if (!canonical_path) return NULL;
    /* Handle stdlib:// and dep:// sentinels: leave them untouched (the
     * client may not resolve them but they round-trip safely). */
    if (strncmp(canonical_path, "stdlib://", 9) == 0 ||
        strncmp(canonical_path, "dep://",    6) == 0 ||
        strncmp(canonical_path, "file://",   7) == 0) {
        return iron_arena_strdup(arena, canonical_path, strlen(canonical_path));
    }
    /* RFC 8089: `file:///abs/path`, and on Windows `file:///C:/dir/f.iron`
     * (`file://C:/...` would make `C:` the host, and the editor cannot
     * open it). Backslashes become '/'; bytes a URI cannot hold raw are
     * percent-encoded (a space in `Program Files`, '%', '#', '?'). */
    size_t path_len = strlen(canonical_path);
    char *buf = (char *)iron_arena_alloc(arena, 8 + 1 + path_len * 3 + 1, 1);
    if (!buf) return NULL;
    size_t n = 0;
    memcpy(buf, "file://", 7);
    n = 7;
    if (canonical_path[0] != '/') buf[n++] = '/';
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < path_len; i++) {
        unsigned char c = (unsigned char)canonical_path[i];
        if (c == '\\') {
            buf[n++] = '/';
        } else if (c == ' ' || c == '%' || c == '#' || c == '?' || c < 0x20) {
            buf[n++] = '%';
            buf[n++] = hex[c >> 4];
            buf[n++] = hex[c & 15];
        } else {
            buf[n++] = (char)c;
        }
    }
    buf[n] = '\0';
    return buf;
}

static int uri_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char *ilsp_nav_uri_to_path(const char *uri, Iron_Arena *arena) {
    if (!uri) return NULL;
    if (strncmp(uri, "file://", 7) == 0) {
        const char *p = uri + 7;
        /* file:///C:/... -> C:/... ; file:///home/... -> /home/... */
        if (p[0] == '/' && p[1] && p[2] == ':') p++;
        else if (p[0] == '/' && p[1] && p[2] == '%' && p[3] == '3' &&
                 (p[4] == 'A' || p[4] == 'a')) p++;
        size_t len = strlen(p);
        char *out = (char *)iron_arena_alloc(arena, len + 1, 1);
        if (!out) return NULL;
        size_t n = 0;
        for (size_t i = 0; i < len; i++) {
            int hi, lo;
            if (p[i] == '%' && i + 2 < len && (hi = uri_hex(p[i + 1])) >= 0 &&
                (lo = uri_hex(p[i + 2])) >= 0) {
                out[n++] = (char)(hi * 16 + lo);
                i += 2;
            } else {
                out[n++] = p[i];
            }
        }
        out[n] = '\0';
        return out;
    }
    /* stdlib:// / dep:// / relative: passthrough. */
    return iron_arena_strdup(arena, uri, strlen(uri));
}

/* Phase 10 D-08: see header comment. Body byte-identical to the
 * original src/lsp/facade/edit/rename/apply.c:96-98 implementation
 * (lifted verbatim with the new public name). */
bool ilsp_nav_path_is_stdlib(const char *p) {
    if (!p) return false;
    if (strncmp(p, "stdlib://", 9) == 0) return true;
    /* A file of the stdlib prelude this thread's last analysis prepended
     * (the facade analyzes and then answers on the same thread). */
    return iron_stdlib_origin_classify(p) == 1;
}

/* ── Member and type references ──────────────────────────────────── */

static const Iron_Type *strip_wrappers(const Iron_Type *t) {
    for (int guard = 0; t && guard < 8; guard++) {
        switch ((int)t->kind) {
            case IRON_TYPE_NULLABLE: t = t->nullable.inner; continue;
            case IRON_TYPE_RC:       t = t->rc.inner;       continue;
            case IRON_TYPE_WEAK_RC:  t = t->weak_rc.inner;  continue;
            case IRON_TYPE_PTR:      t = t->ptr.pointee;    continue;
            default: return t;
        }
    }
    return t;
}

/* Every declaration the analysis saw: the buffer's, then the prelude's. */
static int all_decl_count(const Iron_Program *program) {
    return program->decl_count + program->prelude_decl_count;
}

static Iron_Node *find_type_decl(const Iron_Program *program, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < all_decl_count(program); i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        const char *dn = NULL;
        if (d->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            if (od->is_patch) continue;
            dn = od->name;
        } else if (d->kind == IRON_NODE_ENUM_DECL) {
            dn = ((Iron_EnumDecl *)d)->name;
        } else if (d->kind == IRON_NODE_INTERFACE_DECL) {
            dn = ((Iron_InterfaceDecl *)d)->name;
        }
        if (dn && strcmp(dn, name) == 0) return d;
    }
    return NULL;
}

/* The name methods are declared under for a receiver type: `Point`,
 * `String`, `Int`, `Map` (generic arguments dropped). */
static const char *receiver_type_name(const Iron_Type *t, Iron_Arena *arena) {
    if (!t) return NULL;
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl) return t->object.decl->name;
    if (t->kind == IRON_TYPE_ENUM && t->enu.decl) return t->enu.decl->name;
    if (t->kind == IRON_TYPE_INTERFACE && t->interface.decl) return t->interface.decl->name;
    const char *s = iron_type_to_string(t, arena);
    if (!s) return NULL;
    const char *br = strchr(s, '[');
    if (!br || br == s) return s;
    return iron_arena_strdup(arena, s, (size_t)(br - s));
}

/* The type a static member reference names (`Math.sqrt`, `Color.Red`). */
static Iron_Node *static_receiver_decl(Iron_Node *object) {
    if (!object || object->kind != IRON_NODE_IDENT) return NULL;
    const Iron_Symbol *sym = ((Iron_Ident *)object)->resolved_sym;
    if (!sym || !sym->decl_node) return NULL;
    Iron_NodeKind k = sym->decl_node->kind;
    if (k == IRON_NODE_OBJECT_DECL || k == IRON_NODE_ENUM_DECL ||
        k == IRON_NODE_INTERFACE_DECL) {
        return sym->decl_node;
    }
    return NULL;
}

static const char *decl_type_name(Iron_Node *d) {
    switch ((int)d->kind) {
        case IRON_NODE_OBJECT_DECL:    return ((Iron_ObjectDecl *)d)->name;
        case IRON_NODE_ENUM_DECL:      return ((Iron_EnumDecl *)d)->name;
        case IRON_NODE_INTERFACE_DECL: return ((Iron_InterfaceDecl *)d)->name;
        default: return NULL;
    }
}

static Iron_Node *find_method(const Iron_Program *program, const Iron_Type *recv,
                              const char *type_name, const char *method) {
    if (recv && recv->kind == IRON_TYPE_INTERFACE && recv->interface.decl) {
        Iron_InterfaceDecl *id = recv->interface.decl;
        for (int i = 0; i < id->method_count; i++) {
            Iron_Node *sig = id->method_sigs[i];
            if (sig && sig->kind == IRON_NODE_FUNC_DECL &&
                ((Iron_FuncDecl *)sig)->name &&
                strcmp(((Iron_FuncDecl *)sig)->name, method) == 0) {
                return sig;
            }
        }
    }
    bool is_list = recv && recv->kind == IRON_TYPE_ARRAY;
    for (int i = 0; i < all_decl_count(program); i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->method_name || strcmp(md->method_name, method) != 0) continue;
        if (is_list ? md->is_array_extension
                    : (!md->is_array_extension && md->type_name && type_name &&
                       strcmp(md->type_name, type_name) == 0)) {
            return d;
        }
    }
    return NULL;
}

Iron_Node *ilsp_nav_member_decl(const Iron_Program *program, Iron_Node *n,
                                Iron_Arena *arena) {
    if (!program || !n) return NULL;
    switch ((int)n->kind) {
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            if (!mc->method) return NULL;
            Iron_Node *st = static_receiver_decl(mc->object);
            if (st) return find_method(program, NULL, decl_type_name(st), mc->method);
            const Iron_Type *t = strip_wrappers(
                mc->object ? ((Iron_ExprNode *)mc->object)->resolved_type : NULL);
            if (!t) return NULL;
            return find_method(program, t, receiver_type_name(t, arena), mc->method);
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
            if (!fa->field) return NULL;
            Iron_Node *owner = static_receiver_decl(fa->object);
            if (!owner) {
                const Iron_Type *t = strip_wrappers(
                    fa->object ? ((Iron_ExprNode *)fa->object)->resolved_type : NULL);
                if (t && t->kind == IRON_TYPE_OBJECT) owner = (Iron_Node *)t->object.decl;
            }
            if (!owner) return NULL;
            if (owner->kind == IRON_NODE_OBJECT_DECL) {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)owner;
                for (int i = 0; i < od->field_count; i++) {
                    Iron_Field *f = (Iron_Field *)od->fields[i];
                    if (f && f->name && strcmp(f->name, fa->field) == 0) return (Iron_Node *)f;
                }
            } else if (owner->kind == IRON_NODE_ENUM_DECL) {
                Iron_EnumDecl *ed = (Iron_EnumDecl *)owner;
                for (int i = 0; i < ed->variant_count; i++) {
                    Iron_EnumVariant *v = (Iron_EnumVariant *)ed->variants[i];
                    if (v && v->name && strcmp(v->name, fa->field) == 0) return (Iron_Node *)v;
                }
            }
            return NULL;
        }
        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)n;
            Iron_Node *ed = find_type_decl(program, ec->enum_name);
            if (!ed && ec->resolved_type && ec->resolved_type->kind == IRON_TYPE_ENUM) {
                ed = (Iron_Node *)ec->resolved_type->enu.decl;  /* `.Circle` */
            }
            if (!ed || ed->kind != IRON_NODE_ENUM_DECL || !ec->variant_name) return NULL;
            Iron_EnumDecl *e = (Iron_EnumDecl *)ed;
            for (int i = 0; i < e->variant_count; i++) {
                Iron_EnumVariant *v = (Iron_EnumVariant *)e->variants[i];
                if (v && v->name && strcmp(v->name, ec->variant_name) == 0) return (Iron_Node *)v;
            }
            return NULL;
        }
        case IRON_NODE_TYPE_ANNOTATION:
            return find_type_decl(program, ((Iron_TypeAnnotation *)n)->name);
        default:
            return NULL;
    }
}

typedef struct { const Iron_Node *decl; Iron_Symbol *sym; } SymFind;

static bool sym_find_visit(Iron_Visitor *v, Iron_Node *n) {
    SymFind *f = (SymFind *)v->ctx;
    if (f->sym || n->kind == IRON_NODE_ERROR) return false;
    if (n->kind == IRON_NODE_IDENT && ((Iron_Ident *)n)->resolved_sym &&
        ((Iron_Ident *)n)->resolved_sym->decl_node == f->decl) {
        f->sym = ((Iron_Ident *)n)->resolved_sym;
        return false;
    }
    return true;
}

Iron_Symbol *ilsp_nav_symbol_of_decl(const Iron_Program *program, const Iron_Node *decl) {
    if (!program || !decl) return NULL;
    SymFind f = { decl, NULL };
    Iron_Visitor v = { .ctx = &f, .visit_node = sym_find_visit, .post_visit = NULL };
    for (int i = 0; i < program->decl_count && !f.sym; i++) {
        if (program->decls[i]) iron_ast_walk(program->decls[i], &v);
    }
    return f.sym;
}

static const char *decl_own_name(const Iron_Node *d) {
    switch ((int)d->kind) {
        case IRON_NODE_FUNC_DECL:      return ((const Iron_FuncDecl *)d)->name;
        case IRON_NODE_METHOD_DECL:    return ((const Iron_MethodDecl *)d)->method_name;
        case IRON_NODE_OBJECT_DECL:    return ((const Iron_ObjectDecl *)d)->name;
        case IRON_NODE_ENUM_DECL:      return ((const Iron_EnumDecl *)d)->name;
        case IRON_NODE_INTERFACE_DECL: return ((const Iron_InterfaceDecl *)d)->name;
        case IRON_NODE_ENUM_VARIANT:   return ((const Iron_EnumVariant *)d)->name;
        case IRON_NODE_FIELD:          return ((const Iron_Field *)d)->name;
        case IRON_NODE_PARAM:          return ((const Iron_Param *)d)->name;
        case IRON_NODE_VAL_DECL:       return ((const Iron_ValDecl *)d)->name;
        case IRON_NODE_VAR_DECL:       return ((const Iron_VarDecl *)d)->name;
        default:                       return NULL;
    }
}

static bool name_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

Iron_Span ilsp_nav_decl_name_span(const Iron_Node *decl, const char *text,
                                  size_t text_len) {
    Iron_Span sp = decl ? decl->span : (Iron_Span){0};
    const char *name = decl ? decl_own_name(decl) : NULL;
    if (!name || !*name || !text || sp.line == 0 || sp.col == 0) return sp;
    /* Byte offset of the span's start: walk to its line. */
    size_t off = 0;
    for (uint32_t l = 1; l < sp.line && off < text_len; off++) {
        if (text[off] == '\n') l++;
    }
    off += sp.col - 1;
    size_t n = strlen(name);
    /* `func Type.name`: the method name follows the type and the dot. */
    for (size_t i = off; i + n <= text_len && text[i] != '\n'; i++) {
        if (memcmp(text + i, name, n) != 0) continue;
        if (i > 0 && name_byte(text[i - 1])) continue;
        if (i + n < text_len && name_byte(text[i + n])) continue;
        if (decl->kind == IRON_NODE_METHOD_DECL && i > 0 && text[i - 1] != '.' &&
            text[i - 1] != ' ' && text[i - 1] != '\t') continue;
        Iron_Span r = sp;
        r.col = sp.col + (uint32_t)(i - off);
        r.end_line = sp.line;
        r.end_col = r.col + (uint32_t)n - 1;
        return r;
    }
    return sp;
}

/* ── Names in a document and what they name ──────────────────────── */

typedef struct {
    const IronLsp_Document *doc;
    const Iron_Program     *program;
    Iron_Arena             *arena;
    IronLsp_NameOcc        *occs;
    size_t                  n, cap;
    struct { uint64_t key; bool value; } *seen;  /* stb_ds: line << 32 | col */
} OccCtx;

/* Byte offset of a 1-based (line, col) in the document; SIZE_MAX when
 * outside it. */
static size_t occ_offset(const IronLsp_Document *doc, uint32_t line, uint32_t col) {
    if (line == 0 || col == 0) return SIZE_MAX;
    size_t ls = ilsp_byte_of_line(&doc->line_idx, line - 1);
    if (ls > doc->text_len) return SIZE_MAX;
    size_t off = ls + col - 1;
    return off <= doc->text_len ? off : SIZE_MAX;
}

static size_t occ_line_end(const IronLsp_Document *doc, size_t off) {
    while (off < doc->text_len && doc->text[off] != '\n') off++;
    return off;
}

static bool occ_word_at(const IronLsp_Document *doc, size_t off, const char *name) {
    size_t n = strlen(name);
    if (n == 0 || off + n > doc->text_len) return false;
    if (memcmp(doc->text + off, name, n) != 0) return false;
    if (off > 0 && name_byte(doc->text[off - 1])) return false;
    if (off + n < doc->text_len && name_byte(doc->text[off + n])) return false;
    return true;
}

/* The first whole-word `name` in [from, limit), or SIZE_MAX. */
static size_t occ_find(const IronLsp_Document *doc, size_t from, size_t limit,
                       const char *name) {
    size_t n = strlen(name);
    if (limit > doc->text_len) limit = doc->text_len;
    for (size_t i = from; i != SIZE_MAX && i + n <= limit; i++) {
        if (occ_word_at(doc, i, name)) return i;
    }
    return SIZE_MAX;
}

static void occ_push(OccCtx *c, size_t off, const Iron_Node *decl, const char *name,
                     bool is_decl) {
    if (off == SIZE_MAX || !decl || !name || !*name) return;
    uint32_t line0 = ilsp_line_of_byte(&c->doc->line_idx, off);
    size_t ls = ilsp_byte_of_line(&c->doc->line_idx, line0);
    Iron_Span sp = { 0 };
    sp.filename = c->doc->uri;
    sp.line = sp.end_line = line0 + 1;
    sp.col = (uint32_t)(off - ls) + 1;
    sp.end_col = sp.col + (uint32_t)strlen(name) - 1;
    /* Generic instances repeat nodes: one occurrence per position. */
    uint64_t key = ((uint64_t)sp.line << 32) | sp.col;
    if (hmgeti(c->seen, key) >= 0) return;
    hmput(c->seen, key, true);
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 64;
        IronLsp_NameOcc *no = (IronLsp_NameOcc *)iron_arena_alloc(
            c->arena, ncap * sizeof(*no), _Alignof(IronLsp_NameOcc));
        if (!no) return;
        if (c->n) memcpy(no, c->occs, c->n * sizeof(*no));
        c->occs = no;
        c->cap = ncap;
    }
    IronLsp_NameOcc *o = &c->occs[c->n++];
    o->span = sp;
    o->decl = decl;
    o->name = name;
    o->is_decl = is_decl;
}

/* `.name` after `object` (spaces and line breaks allowed around the
 * dot): the offset of `name`, or SIZE_MAX. */
static size_t occ_member(const IronLsp_Document *doc, const Iron_Node *object,
                         const char *name) {
    if (!object || object->span.line == 0 || !name) return SIZE_MAX;
    size_t i = occ_offset(doc, object->span.end_line, object->span.end_col);
    if (i == SIZE_MAX) return SIZE_MAX;
    i++;
    while (i < doc->text_len && (doc->text[i] == ' ' || doc->text[i] == '\t' ||
                                 doc->text[i] == '\n' || doc->text[i] == '\r')) i++;
    if (i >= doc->text_len || doc->text[i] != '.') return SIZE_MAX;
    i++;
    while (i < doc->text_len && (doc->text[i] == ' ' || doc->text[i] == '\t')) i++;
    return occ_word_at(doc, i, name) ? i : SIZE_MAX;
}

static Iron_Node *variant_of(Iron_Node *enum_decl, const char *variant) {
    if (!enum_decl || enum_decl->kind != IRON_NODE_ENUM_DECL || !variant) return NULL;
    Iron_EnumDecl *ed = (Iron_EnumDecl *)enum_decl;
    for (int i = 0; i < ed->variant_count; i++) {
        Iron_EnumVariant *v = (Iron_EnumVariant *)ed->variants[i];
        if (v && v->name && strcmp(v->name, variant) == 0) return (Iron_Node *)v;
    }
    return NULL;
}

/* `Shape.Circle` in an enum construction or a pattern starting at
 * `start`: the enum's name and the variant's. Returns the offset after
 * the variant name (or `start`). */
static size_t occ_enum_ref(OccCtx *c, size_t start, size_t limit, const char *enum_name,
                           const char *variant, Iron_Node *variant_decl) {
    Iron_Node *ed = find_type_decl(c->program, enum_name);
    size_t at = start;
    if (enum_name && ed && occ_word_at(c->doc, start, enum_name)) {
        occ_push(c, start, ed, ed->kind == IRON_NODE_ENUM_DECL
                                   ? ((Iron_EnumDecl *)ed)->name : enum_name, false);
        at = start + strlen(enum_name);
    }
    if (variant && variant_decl) {
        size_t v = occ_find(c->doc, at, limit, variant);
        if (v != SIZE_MAX) {
            occ_push(c, v, variant_decl, ((Iron_EnumVariant *)variant_decl)->name, false);
            return v + strlen(variant);
        }
    }
    return at;
}

static void occ_pattern(OccCtx *c, Iron_Pattern *p) {
    size_t start = occ_offset(c->doc, p->span.line, p->span.col);
    if (start == SIZE_MAX) return;
    size_t end = occ_offset(c->doc, p->span.end_line, p->span.end_col);
    size_t eol = occ_line_end(c->doc, start);
    size_t limit = (end != SIZE_MAX && end + 1 > eol) ? end + 1 : eol;
    Iron_Node *ed = find_type_decl(c->program, p->enum_name);
    size_t at = occ_enum_ref(c, start, limit, p->enum_name, p->variant_name,
                             variant_of(ed, p->variant_name));
    for (int i = 0; i < p->binding_count; i++) {
        const char *bn = p->binding_names ? p->binding_names[i] : NULL;
        if (!bn) continue;  /* `_` or a nested pattern, walked on its own */
        size_t b = occ_find(c->doc, at, limit, bn);
        if (b == SIZE_MAX) continue;
        occ_push(c, b, (Iron_Node *)p, bn, true);
        at = b + strlen(bn);
    }
}

static bool occ_visit(Iron_Visitor *v, Iron_Node *n) {
    OccCtx *c = (OccCtx *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->span.line == 0) return true;
    if (n->span.filename && c->doc->uri && strcmp(n->span.filename, c->doc->uri) != 0)
        return true;
    const IronLsp_Document *doc = c->doc;
    switch ((int)n->kind) {
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)n;
            Iron_Symbol *sym = id->resolved_sym;
            if (!sym || !sym->decl_node || !id->name) break;
            size_t off = occ_offset(doc, id->span.line, id->span.col);
            if (off == SIZE_MAX || !occ_word_at(doc, off, id->name)) break;
            occ_push(c, off, sym->decl_node, id->name, false);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
            Iron_Node *d = ilsp_nav_member_decl(c->program, n, c->arena);
            if (d) occ_push(c, occ_member(doc, fa->object, fa->field), d, fa->field, false);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            Iron_Node *d = ilsp_nav_member_decl(c->program, n, c->arena);
            if (d) occ_push(c, occ_member(doc, mc->object, mc->method), d, mc->method, false);
            break;
        }
        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)n;
            size_t start = occ_offset(doc, n->span.line, n->span.col);
            if (start == SIZE_MAX) break;
            occ_enum_ref(c, start, occ_line_end(doc, start), ec->enum_name, ec->variant_name,
                         ilsp_nav_member_decl(c->program, n, c->arena));
            break;
        }
        case IRON_NODE_PATTERN:
            occ_pattern(c, (Iron_Pattern *)n);
            break;
        case IRON_NODE_TYPE_ANNOTATION: {
            Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)n;
            Iron_Node *d = ta->name ? find_type_decl(c->program, ta->name) : NULL;
            if (!d) break;
            size_t start = occ_offset(doc, n->span.line, n->span.col);
            if (start == SIZE_MAX) break;
            occ_push(c, occ_find(doc, start, occ_line_end(doc, start), ta->name), d,
                     decl_type_name(d), false);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)n;
            size_t start = occ_offset(doc, n->span.line, n->span.col);
            if (start == SIZE_MAX) break;
            size_t eol = occ_line_end(doc, start);
            size_t at = start + 3;  /* after `for` */
            const char *names[2] = { fs->var_name, fs->var_name2 };
            for (int i = 0; i < 2; i++) {
                if (!names[i] || strcmp(names[i], "_") == 0) continue;
                size_t o = occ_find(doc, at, eol, names[i]);
                if (o == SIZE_MAX) break;
                occ_push(c, o, n, names[i], true);
                at = o + strlen(names[i]);
            }
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)n;
            if (vd->binding_count > 0) {
                size_t start = occ_offset(doc, n->span.line, n->span.col);
                if (start == SIZE_MAX) break;
                size_t eol = occ_line_end(doc, start), at = start;
                for (int i = 0; i < vd->binding_count; i++) {
                    const char *bn = vd->binding_names ? vd->binding_names[i] : NULL;
                    if (!bn) continue;
                    size_t o = occ_find(doc, at, eol, bn);
                    if (o == SIZE_MAX) break;
                    occ_push(c, o, n, bn, true);
                    at = o + strlen(bn);
                }
                break;
            }
        }
            /* fall through */
        case IRON_NODE_VAR_DECL:
        case IRON_NODE_FUNC_DECL:
        case IRON_NODE_METHOD_DECL:
        case IRON_NODE_OBJECT_DECL:
        case IRON_NODE_ENUM_DECL:
        case IRON_NODE_INTERFACE_DECL:
        case IRON_NODE_FIELD:
        case IRON_NODE_ENUM_VARIANT:
        case IRON_NODE_PARAM: {
            const char *name = decl_own_name(n);
            if (!name || !*name || strcmp(name, "self") == 0) break;
            if (n->kind == IRON_NODE_OBJECT_DECL && ((Iron_ObjectDecl *)n)->is_patch) break;
            /* The name, when written on the line the declaration starts
             * (as ilsp_nav_decl_name_span finds it, through the line
             * index rather than a scan from the top of the file). */
            size_t start = occ_offset(doc, n->span.line, n->span.col);
            if (start == SIZE_MAX) break;
            size_t eol = occ_line_end(doc, start), off = start;
            for (;;) {
                off = occ_find(doc, off, eol, name);
                if (off == SIZE_MAX) break;
                /* `func Type.name`: the method name follows the type and
                 * the dot, or a space. */
                if (n->kind != IRON_NODE_METHOD_DECL || off == 0 || doc->text[off - 1] == '.' ||
                    doc->text[off - 1] == ' ' || doc->text[off - 1] == '\t') break;
                off += strlen(name);
            }
            if (off == SIZE_MAX) break;
            /* `func Point.area()`: the receiver type's name too. */
            if (n->kind == IRON_NODE_METHOD_DECL) {
                Iron_MethodDecl *md = (Iron_MethodDecl *)n;
                if (!md->is_array_extension && md->type_name && off >= 1 &&
                    doc->text[off - 1] == '.') {
                    size_t tl = strlen(md->type_name);
                    Iron_Node *td = find_type_decl(c->program, md->type_name);
                    if (td && off >= 1 + tl && occ_word_at(doc, off - 1 - tl, md->type_name)) {
                        occ_push(c, off - 1 - tl, td, decl_type_name(td), false);
                    }
                }
            }
            occ_push(c, off, n, name, true);
            break;
        }
        default:
            break;
    }
    return true;
}

IronLsp_NameOcc *ilsp_nav_name_occurrences(const IronLsp_Document *doc,
                                           const Iron_Program     *program,
                                           Iron_Arena             *arena,
                                           size_t                 *out_n) {
    if (out_n) *out_n = 0;
    if (!doc || !doc->text || !program || !arena) return NULL;
    OccCtx c = { doc, program, arena, NULL, 0, 0, NULL };
    Iron_Visitor v = { .ctx = &c, .visit_node = occ_visit, .post_visit = NULL };
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i]) iron_ast_walk(program->decls[i], &v);
    }
    hmfree(c.seen);
    if (out_n) *out_n = c.n;
    return c.occs;
}

const IronLsp_NameOcc *ilsp_nav_occurrence_at(const IronLsp_NameOcc   *occs,
                                              size_t                   n,
                                              const IronLsp_Document  *doc,
                                              IronLsp_Position         pos,
                                              IronLsp_PositionEncoding enc) {
    if (!occs || !doc || !doc->text) return NULL;
    size_t ls = ilsp_byte_of_line(&doc->line_idx, pos.line);
    if (ls > doc->text_len) return NULL;
    size_t le = occ_line_end(doc, ls);
    size_t b = enc == ILSP_ENC_UTF16
        ? ilsp_utf16_column_to_utf8_byte(doc->text + ls, le - ls, pos.character)
        : ilsp_utf8_column_to_utf8_byte(doc->text + ls, le - ls, pos.character);
    uint32_t line = pos.line + 1, col = (uint32_t)b + 1;
    const IronLsp_NameOcc *after = NULL;
    for (size_t i = 0; i < n; i++) {
        const Iron_Span *s = &occs[i].span;
        if (s->line != line) continue;
        if (col >= s->col && col <= s->end_col) return &occs[i];
        if (col == s->end_col + 1) after = &occs[i];  /* the cursor right after it */
    }
    return after;
}

bool ilsp_nav_occ_same(const IronLsp_NameOcc *a, const Iron_Node *decl, const char *name) {
    return a && a->decl == decl && a->name && name && strcmp(a->name, name) == 0;
}

/* ── LocationLink -> JSON ────────────────────────────────────────── */

yyjson_mut_val *ilsp_nav_build_location_link_json(
    yyjson_mut_doc             *doc,
    const IronLsp_LocationLink *link,
    bool                        client_supports_link) {
    if (!doc || !link) return NULL;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);

    if (client_supports_link) {
        /* LocationLink shape: originSelectionRange?, targetUri,
         * targetRange, targetSelectionRange. */
        attach_range_local(doc, obj, "originSelectionRange",
                            link->origin_selection_range);
        yyjson_mut_obj_add_strcpy(doc, obj, "targetUri",
                                    link->target_uri ? link->target_uri : "");
        attach_range_local(doc, obj, "targetRange",          link->target_range);
        attach_range_local(doc, obj, "targetSelectionRange", link->target_selection_range);
    } else {
        /* Location shape: { uri, range }. */
        yyjson_mut_obj_add_strcpy(doc, obj, "uri",
                                    link->target_uri ? link->target_uri : "");
        attach_range_local(doc, obj, "range", link->target_range);
    }
    return obj;
}

yyjson_mut_val *ilsp_nav_build_location_link_array(
    yyjson_mut_doc                  *doc,
    const IronLsp_LocationLink      *links,
    size_t                           n,
    bool                             client_supports_link) {
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *obj = ilsp_nav_build_location_link_json(
            doc, &links[i], client_supports_link);
        if (obj) yyjson_mut_arr_append(arr, obj);
    }
    return arr;
}

/* ── Response envelope ───────────────────────────────────────────── */

static yyjson_mut_val *clone_id_local(yyjson_mut_doc *rd, yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return yyjson_mut_null(rd);
    if (yyjson_is_int(id) || yyjson_is_sint(id))
        return yyjson_mut_sint(rd, yyjson_get_sint(id));
    if (yyjson_is_uint(id))
        return yyjson_mut_uint(rd, yyjson_get_uint(id));
    if (yyjson_is_str(id))
        return yyjson_mut_strcpy(rd, yyjson_get_str(id));
    return yyjson_mut_null(rd);
}

yyjson_mut_val *ilsp_nav_wrap_response(yyjson_mut_doc   *doc,
                                        yyjson_val       *request_id,
                                        yyjson_mut_val   *result_val) {
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, root, "id", clone_id_local(doc, request_id));
    yyjson_mut_obj_add_val(doc, root, "result",
                            result_val ? result_val : yyjson_mut_null(doc));
    return root;
}
