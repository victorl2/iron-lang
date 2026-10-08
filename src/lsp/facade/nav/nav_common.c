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
    size_t path_len = strlen(canonical_path);
    size_t total    = path_len + 8;  /* "file://" + NUL */
    char *buf = (char *)iron_arena_alloc(arena, total, 1);
    if (!buf) return NULL;
    memcpy(buf, "file://", 7);
    memcpy(buf + 7, canonical_path, path_len);
    buf[7 + path_len] = '\0';
    return buf;
}

const char *ilsp_nav_uri_to_path(const char *uri, Iron_Arena *arena) {
    if (!uri) return NULL;
    if (strncmp(uri, "file://", 7) == 0) {
        const char *p = uri + 7;
        return iron_arena_strdup(arena, p, strlen(p));
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
        case IRON_NODE_TYPE_ANNOTATION:
            return find_type_decl(program, ((Iron_TypeAnnotation *)n)->name);
        default:
            return NULL;
    }
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
