/* textDocument/signatureHelp.
 *
 * The call whose argument list holds the cursor is the one with the
 * latest `(` before it whose `)` comes after it or is not typed yet, so
 * `area(1, |` gets help while the line is still being written, and
 * calls inside lambdas, interpolations and list literals are found like
 * any other. The active parameter counts the top-level commas between
 * the `(` and the cursor, outside brackets and strings.
 *
 * Signatures come from the declaration the call resolves to (a function,
 * a method in the file or the stdlib, an interface signature), from an
 * object's `init`s or fields for a construction, from builtin_members.h
 * for `println`, `xs.push`, `m.put`..., or from the type of a binding
 * holding a function. A method's implicit `self` is not shown.
 */

#include "lsp/facade/builtin_members.h"
#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/utf.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "lsp/facade/nav/nav_common.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small string-builder bound to an arena. */
typedef struct {
    char        *buf;
    size_t       len;
    size_t       cap;
    Iron_Arena  *arena;
} SB;

static void sb_init(SB *sb, Iron_Arena *arena) {
    sb->arena = arena;
    sb->cap = 128;
    sb->len = 0;
    sb->buf = (char *)iron_arena_alloc(arena, sb->cap, 1);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_grow(SB *sb, size_t need) {
    if (sb->len + need + 1 <= sb->cap) return;
    size_t ncap = sb->cap;
    while (ncap < sb->len + need + 1) ncap *= 2;
    char *nb = (char *)iron_arena_alloc(sb->arena, ncap, 1);
    if (!nb) return;
    memcpy(nb, sb->buf, sb->len + 1);
    sb->buf = nb; sb->cap = ncap;
}

static void sb_append(SB *sb, const char *s) {
    if (!sb->buf || !s) return;
    size_t l = strlen(s);
    sb_grow(sb, l);
    if (!sb->buf) return;
    memcpy(sb->buf + sb->len, s, l);
    sb->len += l;
    sb->buf[sb->len] = '\0';
}

/* Render a type annotation node as a string. */
static const char *render_type_ann(Iron_Node *n, Iron_Arena *arena) {
    if (!n) return "Void";
    if (n->kind == IRON_NODE_TYPE_ANNOTATION) {
        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)n;
        SB tmp; sb_init(&tmp, arena);
        if (ta->is_array) sb_append(&tmp, "[");
        sb_append(&tmp, ta->name ? ta->name : "Unknown");
        if (ta->is_array) sb_append(&tmp, "]");
        if (ta->is_nullable) sb_append(&tmp, "?");
        return tmp.buf ? tmp.buf : "Unknown";
    }
    return "Unknown";
}

/* A parameter's type: the checker's, when it ran (`func(Int) -> Int`
 * for a lambda parameter), else as written. */
static const char *param_type_str(Iron_Param *pm, Iron_Arena *arena) {
    if (pm->resolved_type && pm->resolved_type->kind != IRON_TYPE_ERROR) {
        const char *s = iron_type_to_string(pm->resolved_type, arena);
        if (s) return s;
    }
    return render_type_ann(pm->type_ann, arena);
}

/* Build a SignatureInfo for a function/method decl. Returns the info
 * with parameter_offsets populated. `self_type` is NULL for free
 * funcs; non-NULL prepends `self: <type>` as the first parameter. */
static void build_sig_info(IronLsp_SignatureInfo *out,
                             Iron_FuncDecl         *fd,
                             Iron_MethodDecl       *md,
                             const char            *self_type,
                             Iron_Arena            *arena) {
    memset(out, 0, sizeof(*out));
    SB label; sb_init(&label, arena);

    /* NEW Phase 10 TIER-04: prefix tier modifier on signature label.
     * Mutual exclusion of is_readonly + is_pure is parser-enforced
     * (parser.c:3162-3180), so at most one prefix word is emitted.
     * parameter_offsets math is computed from the open-paren position
     * which is AFTER the `func ` token; the prefix does NOT shift
     * the offsets. */
    bool ro = (md ? md->is_readonly : (fd ? fd->is_readonly : false));
    bool pu = (md ? md->is_pure     : (fd ? fd->is_pure     : false));
    if (ro)      sb_append(&label, "readonly ");
    else if (pu) sb_append(&label, "pure ");

    /* Prefix: "func Name(" or "func Container.Name(". */
    sb_append(&label, "func ");
    if (md && md->is_array_extension) {
        /* `func [T].map(...)`, not the internal `__Array`. */
        sb_append(&label, "[");
        sb_append(&label, md->elem_type_name ? md->elem_type_name : "T");
        sb_append(&label, "].");
        sb_append(&label, md->method_name ? md->method_name : "_");
    } else if (md) {
        sb_append(&label, md->type_name ? md->type_name : "_");
        sb_append(&label, ".");
        sb_append(&label, md->method_name ? md->method_name : "_");
    } else if (fd) {
        sb_append(&label, fd->name ? fd->name : "_");
    } else {
        return;
    }
    sb_append(&label, "(");

    /* Count total params (self + user). */
    int user_count = md ? md->param_count : (fd ? fd->param_count : 0);
    Iron_Node **params = md ? md->params : (fd ? fd->params : NULL);
    /* A method's `self` is implicit in Iron source: `s.replace(old, new)`. */
    if (user_count > 0 && params && params[0] && params[0]->kind == IRON_NODE_PARAM &&
        ((Iron_Param *)params[0])->name &&
        strcmp(((Iron_Param *)params[0])->name, "self") == 0) {
        params++;
        user_count--;
    }
    int total = user_count + (self_type ? 1 : 0);

    IronLsp_SigParam *offs = NULL;
    if (total > 0) {
        offs = (IronLsp_SigParam *)iron_arena_alloc(
            arena, (size_t)total * sizeof(*offs), _Alignof(IronLsp_SigParam));
        if (!offs) return;
    }

    int idx = 0;
    if (self_type) {
        int start = (int)label.len;
        sb_append(&label, "self: ");
        sb_append(&label, self_type);
        int end = (int)label.len;
        offs[idx].start = start;
        offs[idx].end   = end;
        idx++;
    }
    for (int i = 0; i < user_count; i++) {
        if (idx > 0) sb_append(&label, ", ");
        int start = (int)label.len;
        Iron_Node *p = params ? params[i] : NULL;
        if (p && p->kind == IRON_NODE_PARAM) {
            Iron_Param *pp = (Iron_Param *)p;
            sb_append(&label, pp->name ? pp->name : "_");
            sb_append(&label, ": ");
            sb_append(&label, param_type_str(pp, arena));
        } else {
            sb_append(&label, "_");
        }
        int end = (int)label.len;
        offs[idx].start = start;
        offs[idx].end   = end;
        idx++;
    }
    sb_append(&label, ")");

    /* Return type. */
    Iron_Node *ret = md ? md->return_type : (fd ? fd->return_type : NULL);
    if (ret) {
        sb_append(&label, " -> ");
        sb_append(&label, render_type_ann(ret, arena));
    }

    out->label = label.buf ? label.buf : "";
    out->documentation = fd ? fd->doc_comment : (md ? md->doc_comment : NULL);
    out->parameter_offsets = offs;
    out->parameter_count   = total;
}

/* ── Byte-offset math for the cursor + call paren walk ───────────── */

/* Convert an LSP Position to a byte offset into doc->text. */
static size_t pos_to_byte(const IronLsp_Document *doc,
                           IronLsp_Position pos,
                           IronLsp_PositionEncoding enc) {
    if (!doc || !doc->text) return 0;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, pos.line);
    if (line_start > doc->text_len) return doc->text_len;
    size_t line_end = line_start;
    while (line_end < doc->text_len && doc->text[line_end] != '\n') line_end++;
    const char *line = doc->text + line_start;
    size_t line_len = line_end - line_start;
    size_t byte;
    if (enc == ILSP_ENC_UTF16) {
        byte = ilsp_utf16_column_to_utf8_byte(line, line_len, pos.character);
    } else {
        byte = ilsp_utf8_column_to_utf8_byte(line, line_len, pos.character);
    }
    if (byte > line_len) byte = line_len;
    return line_start + byte;
}

/* Byte offset of a 1-based (line, col) position in doc->text. */
static size_t line_col_to_byte(const IronLsp_Document *doc, uint32_t line, uint32_t col) {
    if (!doc || !doc->text || line == 0) return 0;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, line - 1);
    if (line_start > doc->text_len) return doc->text_len;
    size_t r = line_start + (col > 0 ? col - 1 : 0);
    return r > doc->text_len ? doc->text_len : r;
}

/* The byte after a string literal starting at `i` (a `"`). */
static size_t skip_string(const char *t, size_t len, size_t i) {
    for (i++; i < len; i++) {
        if (t[i] == '\\') { i++; continue; }
        if (t[i] == '"') return i + 1;
        if (t[i] == '\n') return i;
    }
    return len;
}

/* The `(` opening a call's arguments: the first one at or after `from`
 * outside brackets (`Map[String, Int](`) and strings. SIZE_MAX if the
 * next code is anything but the callee, `.name`, `[...]` and spaces. */
static size_t open_paren_from(const IronLsp_Document *doc, size_t from) {
    const char *t = doc->text;
    size_t len = doc->text_len;
    int depth = 0;
    for (size_t i = from; i < len; i++) {
        char c = t[i];
        if (c == '[') { depth++; continue; }
        if (c == ']') { if (depth > 0) depth--; continue; }
        if (depth > 0) continue;
        if (c == '(') return i;
        if (c == '"' || c == ')' || c == '{' || c == '}' || c == ',' || c == '\n' ||
            c == '=') return SIZE_MAX;
    }
    return SIZE_MAX;
}

/* The `)` matching the `(` at `open`, or SIZE_MAX when it is not
 * written yet (the call being typed). Strings are skipped. */
static size_t close_paren(const IronLsp_Document *doc, size_t open) {
    const char *t = doc->text;
    size_t len = doc->text_len;
    int depth = 0;
    for (size_t i = open + 1; i < len;) {
        char c = t[i];
        if (c == '"') { i = skip_string(t, len, i); continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) return c == ')' ? i : SIZE_MAX;
            depth--;
        }
        i++;
    }
    return SIZE_MAX;
}

/* The active parameter: the top-level commas between the `(` and the
 * cursor, outside nested brackets and strings. */
static int active_param_between(const IronLsp_Document *doc,
                                   size_t paren_byte,
                                   size_t cursor_byte) {
    if (!doc || !doc->text) return 0;
    const char *t = doc->text;
    size_t limit = cursor_byte < doc->text_len ? cursor_byte : doc->text_len;
    int depth = 0;
    int commas = 0;
    for (size_t i = paren_byte + 1; i < limit;) {
        char c = t[i];
        if (c == '"') { i = skip_string(t, limit, i); continue; }
        switch (c) {
            case '(': case '[': case '{': depth++; break;
            case ')': case ']': case '}': if (depth > 0) depth--; break;
            case ',': if (depth == 0) commas++; break;
            default: break;
        }
        i++;
    }
    return commas;
}

/* The innermost call whose argument list holds the cursor: the one with
 * the latest `(` before the cursor whose `)` is after it (or not typed
 * yet). Calls inside lambdas, interpolations, list literals and every
 * other expression are seen through the generic AST walker. */
typedef struct {
    const IronLsp_Document *doc;
    size_t                  cursor;
    Iron_Node              *best;
    size_t                  best_paren;
} FindCallCtx;

static bool find_call_visit(Iron_Visitor *v, Iron_Node *n) {
    FindCallCtx *c = (FindCallCtx *)v->ctx;
    if (n->kind == IRON_NODE_ERROR) return false;
    if (n->span.line == 0) return true;
    if (n->span.filename && c->doc->uri && strcmp(n->span.filename, c->doc->uri) != 0)
        return true;
    size_t from;
    if (n->kind == IRON_NODE_CALL) {
        Iron_Node *callee = ((Iron_CallExpr *)n)->callee;
        if (!callee || callee->span.line == 0) return true;
        from = line_col_to_byte(c->doc, callee->span.end_line, callee->span.end_col) + 1;
    } else if (n->kind == IRON_NODE_METHOD_CALL) {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
        if (!mc->object || mc->object->span.line == 0 || !mc->method) return true;
        size_t i = line_col_to_byte(c->doc, mc->object->span.end_line,
                                    mc->object->span.end_col) + 1;
        /* `.method`, spaces and newlines allowed around the dot. */
        while (i < c->doc->text_len && (c->doc->text[i] == ' ' || c->doc->text[i] == '\t' ||
                                        c->doc->text[i] == '\n' || c->doc->text[i] == '\r'))
            i++;
        if (i >= c->doc->text_len || c->doc->text[i] != '.') return true;
        i++;
        while (i < c->doc->text_len && (c->doc->text[i] == ' ' || c->doc->text[i] == '\t')) i++;
        size_t ml = strlen(mc->method);
        if (i + ml > c->doc->text_len || memcmp(c->doc->text + i, mc->method, ml) != 0)
            return true;
        from = i + ml;
    } else {
        return true;
    }
    if (from >= c->doc->text_len) return true;
    size_t open = open_paren_from(c->doc, from);
    if (open == SIZE_MAX || open >= c->cursor) return true;
    size_t close = close_paren(c->doc, open);
    if (close != SIZE_MAX && close < c->cursor) return true;
    if (!c->best || open > c->best_paren) {
        c->best = n;
        c->best_paren = open;
    }
    return true;
}

/* ── Signatures without a declaration ─────────────────────────────── */

/* A signature from its label alone (`func push(item: Int)`): the
 * parameters are the comma-separated parts of its first parenthesis. */
static void sig_from_label(IronLsp_SignatureInfo *out, const char *label,
                           const char *doc_comment, Iron_Arena *arena) {
    memset(out, 0, sizeof(*out));
    out->label = label ? label : "";
    out->documentation = doc_comment;
    if (!label) return;
    const char *open = strchr(label, '(');
    if (!open) return;
    int depth = 0, count = 0;
    IronLsp_SigParam tmp[32];
    int start = (int)(open - label) + 1;
    for (const char *p = open + 1; *p && count < 32; p++) {
        if (*p == '(' || *p == '[') depth++;
        else if ((*p == ')' || *p == ']') && depth > 0) depth--;
        else if ((*p == ',' && depth == 0) || (*p == ')' && depth == 0)) {
            int end = (int)(p - label);
            if (end > start) { tmp[count].start = start; tmp[count].end = end; count++; }
            if (*p == ')') break;
            start = end + 1;
            while (label[start] == ' ') start++;
        }
    }
    if (count == 0) return;
    IronLsp_SigParam *offs = (IronLsp_SigParam *)iron_arena_alloc(
        arena, (size_t)count * sizeof(*offs), _Alignof(IronLsp_SigParam));
    if (!offs) return;
    memcpy(offs, tmp, (size_t)count * sizeof(*offs));
    out->parameter_offsets = offs;
    out->parameter_count = count;
}

/* `Point(x: Int, y: Int)`: an object's construction, from its anonymous
 * `init`s or, when it has none, its fields in order. Returns how many
 * signatures were written to `out` (at most `max`). */
static int constructor_sigs(const Iron_Program *program, Iron_ObjectDecl *od,
                            IronLsp_SignatureInfo *out, int max, Iron_Arena *arena) {
    int n = 0;
    for (int i = 0; i < program->decl_count + program->prelude_decl_count && n < max; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->is_init || md->init_name || !md->type_name || !od->name ||
            strcmp(md->type_name, od->name) != 0) continue;
        SB label; sb_init(&label, arena);
        sb_append(&label, od->name);
        sb_append(&label, "(");
        bool first = true;
        for (int j = 0; j < md->param_count; j++) {
            Iron_Param *pm = (Iron_Param *)md->params[j];
            if (!pm || pm->kind != IRON_NODE_PARAM || !pm->name ||
                strcmp(pm->name, "self") == 0) continue;
            if (!first) sb_append(&label, ", ");
            first = false;
            sb_append(&label, pm->name);
            sb_append(&label, ": ");
            sb_append(&label, param_type_str(pm, arena));
        }
        sb_append(&label, ")");
        sig_from_label(&out[n++], label.buf, md->doc_comment ? md->doc_comment
                                                              : od->doc_comment, arena);
    }
    if (n > 0) return n;
    SB label; sb_init(&label, arena);
    sb_append(&label, od->name ? od->name : "_");
    sb_append(&label, "(");
    for (int j = 0; j < od->field_count; j++) {
        Iron_Field *f = (Iron_Field *)od->fields[j];
        if (!f || f->kind != IRON_NODE_FIELD || !f->name) continue;
        if (label.buf && label.buf[label.len - 1] != '(') sb_append(&label, ", ");
        sb_append(&label, f->name);
        sb_append(&label, ": ");
        sb_append(&label, render_type_ann(f->type_ann, arena));
    }
    sb_append(&label, ")");
    sig_from_label(&out[0], label.buf, od->doc_comment, arena);
    return 1;
}

/* `func(Int) -> Bool`: a call through a binding of function type. */
static const char *func_type_label(const char *name, const Iron_Type *t, Iron_Arena *arena) {
    SB label; sb_init(&label, arena);
    sb_append(&label, "func ");
    sb_append(&label, name ? name : "_");
    sb_append(&label, "(");
    for (int i = 0; i < t->func.param_count; i++) {
        if (i > 0) sb_append(&label, ", ");
        const char *ts = iron_type_to_string(t->func.param_types[i], arena);
        sb_append(&label, ts ? ts : "_");
    }
    sb_append(&label, ")");
    if (t->func.return_type && t->func.return_type->kind != IRON_TYPE_VOID) {
        const char *rs = iron_type_to_string(t->func.return_type, arena);
        sb_append(&label, " -> ");
        sb_append(&label, rs ? rs : "_");
    }
    return label.buf;
}

static const Iron_Type *strip_handles(const Iron_Type *t) {
    for (int g = 0; t && g < 8; g++) {
        if (t->kind == IRON_TYPE_NULLABLE) t = t->nullable.inner;
        else if (t->kind == IRON_TYPE_RC) t = t->rc.inner;
        else if (t->kind == IRON_TYPE_WEAK_RC) t = t->weak_rc.inner;
        else if (t->kind == IRON_TYPE_PTR) t = t->ptr.pointee;
        else break;
    }
    return t;
}

/* ── Entry point ─────────────────────────────────────────────────── */

#define ILSP_SIG_MAX 8

void ilsp_facade_signature_help(struct IronLsp_Server    *server,
                                  struct IronLsp_Document  *doc,
                                  IronLsp_Position          pos,
                                  _Atomic bool             *cancel,
                                  Iron_Arena               *arena,
                                  IronLsp_SignatureInfo   **out_sigs,
                                  size_t                   *out_n,
                                  int                      *out_active_sig,
                                  int                      *out_active_param) {
    if (out_sigs) *out_sigs = NULL;
    if (out_n)    *out_n    = 0;
    if (out_active_sig)   *out_active_sig   = 0;
    if (out_active_param) *out_active_param = 0;
    if (!server || !doc || !arena || !doc->text) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags   = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    size_t cursor_byte = pos_to_byte(doc, pos, enc);

    FindCallCtx ctx = { doc, cursor_byte, NULL, 0 };
    Iron_Visitor v = { .ctx = &ctx, .visit_node = find_call_visit, .post_visit = NULL };
    for (int i = 0; i < program->decl_count; i++) {
        if (program->decls[i]) iron_ast_walk(program->decls[i], &v);
    }
    if (!ctx.best) goto done;

    IronLsp_SignatureInfo *sigs = (IronLsp_SignatureInfo *)iron_arena_alloc(
        arena, ILSP_SIG_MAX * sizeof(*sigs), _Alignof(IronLsp_SignatureInfo));
    if (!sigs) goto done;
    int nsig = 0;
    int arg_count = 0;

    if (ctx.best->kind == IRON_NODE_METHOD_CALL) {
        Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)ctx.best;
        arg_count = mc->arg_count;
        /* The method the call resolves to, in the file or the stdlib
         * (String.replace, Math.pow, a user method, an interface's
         * signature). Its receiver is implicit in Iron source, so no
         * `self` parameter is shown. */
        Iron_Node *d = ilsp_nav_member_decl(program, ctx.best, &walk_arena);
        if (d && d->kind == IRON_NODE_METHOD_DECL) {
            build_sig_info(&sigs[nsig++], NULL, (Iron_MethodDecl *)d, NULL, arena);
        } else if (d && d->kind == IRON_NODE_FUNC_DECL) {
            build_sig_info(&sigs[nsig++], (Iron_FuncDecl *)d, NULL, NULL, arena);
        } else if (mc->object && mc->method) {
            /* A compiler builtin: `xs.push(`, `m.put(`. */
            const Iron_Type *rt = strip_handles(((Iron_ExprNode *)mc->object)->resolved_type);
            const char *detail = ilsp_builtin_find(ilsp_builtin_table_for(rt), mc->method);
            if (detail) {
                sig_from_label(&sigs[nsig++], ilsp_builtin_signature(detail, rt, arena),
                               NULL, arena);
            }
        }
    } else {
        Iron_CallExpr *call = (Iron_CallExpr *)ctx.best;
        arg_count = call->arg_count;
        Iron_Node *callee = call->callee;
        if (callee && callee->kind == IRON_NODE_IDENT) {
            Iron_Ident *id = (Iron_Ident *)callee;
            Iron_Symbol *sym = id->resolved_sym;
            Iron_Node *dn = sym ? sym->decl_node : NULL;
            if (dn && dn->kind == IRON_NODE_FUNC_DECL) {
                build_sig_info(&sigs[nsig++], (Iron_FuncDecl *)dn, NULL, NULL, arena);
            } else if (dn && dn->kind == IRON_NODE_METHOD_DECL) {
                build_sig_info(&sigs[nsig++], NULL, (Iron_MethodDecl *)dn, NULL, arena);
            } else if (dn && dn->kind == IRON_NODE_OBJECT_DECL) {
                nsig = constructor_sigs(program, (Iron_ObjectDecl *)dn, sigs, ILSP_SIG_MAX,
                                        arena);
            } else if (!dn && id->name && ilsp_builtin_find(ilsp_builtin_funcs, id->name)) {
                sig_from_label(&sigs[nsig++], ilsp_builtin_find(ilsp_builtin_funcs, id->name),
                               NULL, arena);
            } else {
                const Iron_Type *ft = id->resolved_type ? id->resolved_type
                                                        : (sym ? sym->type : NULL);
                if (ft && ft->kind == IRON_TYPE_FUNC) {
                    sig_from_label(&sigs[nsig++], func_type_label(id->name, ft, arena),
                                   NULL, arena);
                }
            }
        }
    }
    if (nsig == 0) goto done;

    int active_param = active_param_between(doc, ctx.best_paren, cursor_byte);
    /* Several constructors: the first one taking that many arguments. */
    int active_sig = 0;
    int want = active_param + 1 > arg_count ? active_param + 1 : arg_count;
    for (int i = 0; i < nsig; i++) {
        if (sigs[i].parameter_count >= want) { active_sig = i; break; }
    }
    int pc = sigs[active_sig].parameter_count;
    if (pc > 0) {
        if (active_param >= pc) active_param = pc - 1;
    } else {
        active_param = 0;
    }

    *out_sigs = sigs;
    *out_n    = (size_t)nsig;
    if (out_active_sig)   *out_active_sig   = active_sig;
    if (out_active_param) *out_active_param = active_param;

done:
    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
}
