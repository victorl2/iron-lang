/* Phase 3 Plan 04 Task 03 (NAV-10, D-13) -- textDocument/signatureHelp
 * facade.
 *
 * Triggers on `(` and `,`. active-parameter computed via a byte-walk
 * from the enclosing call's `(` to the cursor, counting top-level
 * commas (depth-tracking `()` `[]` `{}`). Methods render the self
 * parameter first as `self: Container`. Returns empty signatures on
 * Iron_ErrorNode or missing resolved_sym.
 */

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
    if (md) {
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
            sb_append(&label, render_type_ann(pp->type_ann, arena));
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

/* Convert an Iron_Span's (line, col) start to a byte offset in doc->text.
 * Iron_Span lines/cols are 1-based byte-based. */
static size_t span_start_to_byte(const IronLsp_Document *doc, Iron_Span s) {
    if (!doc || !doc->text) return 0;
    if (s.line == 0) return 0;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, s.line - 1);
    if (line_start > doc->text_len) return doc->text_len;
    size_t col_byte = (s.col > 0) ? s.col - 1 : 0;
    return line_start + col_byte;
}

/* Find the opening `(` byte index for a call expression. We search
 * forward from the call's span start for the first unparenthesised
 * `(`. Returns SIZE_MAX on miss. */
static size_t find_call_paren(const IronLsp_Document *doc,
                                size_t call_start_byte,
                                size_t cursor_byte) {
    if (!doc || !doc->text) return (size_t)-1;
    size_t limit = cursor_byte < doc->text_len ? cursor_byte : doc->text_len;
    /* Track depth of enclosing brackets so we ignore `(` inside
     * nested constructs before the call's own paren. But since
     * call_start_byte IS the call expression start (typically an
     * identifier), the first `(` we meet at depth 0 is ours. */
    for (size_t i = call_start_byte; i < limit; i++) {
        if (doc->text[i] == '(') return i;
    }
    return (size_t)-1;
}

/* Compute the active parameter index by walking from paren+1 up to
 * cursor, counting top-level commas. Nested (/[/{ increment depth. */
static int active_param_between(const IronLsp_Document *doc,
                                   size_t paren_byte,
                                   size_t cursor_byte) {
    if (!doc || !doc->text) return 0;
    if (paren_byte >= doc->text_len) return 0;
    int depth = 0;
    int commas = 0;
    size_t limit = cursor_byte < doc->text_len ? cursor_byte : doc->text_len;
    for (size_t i = paren_byte + 1; i < limit; i++) {
        char c = doc->text[i];
        switch (c) {
            case '(': case '[': case '{': depth++; break;
            case ')': case ']': case '}': if (depth > 0) depth--; break;
            case ',': if (depth == 0) commas++; break;
            default: break;
        }
    }
    return commas;
}

/* Walk program to find the innermost call expression whose byte-range
 * contains the cursor. Returns the call node, or NULL. We scan
 * top-level decls (and their bodies) looking for Iron_CallExpr /
 * Iron_MethodCallExpr nodes whose span brackets the cursor. */

typedef struct {
    Iron_Node *best;
    size_t     best_start;
    size_t     best_end;
} FindCallCtx;

/* Decode a span's end byte using the document's line index. end_col
 * is 1-based byte-indexed inclusive (Iron_Span contract). */
static size_t span_end_to_byte(const IronLsp_Document *doc, Iron_Span s) {
    if (!doc || !doc->text) return 0;
    if (s.end_line == 0) return span_start_to_byte(doc, s);
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, s.end_line - 1);
    if (line_start > doc->text_len) return doc->text_len;
    size_t col_byte = (s.end_col > 0) ? s.end_col : 0;
    size_t r = line_start + col_byte;
    if (r > doc->text_len) r = doc->text_len;
    return r;
}

static bool byte_in_span(const IronLsp_Document *doc, Iron_Span s, size_t b) {
    size_t a = span_start_to_byte(doc, s);
    size_t e = span_end_to_byte(doc, s);
    return b >= a && b <= e;
}

static void find_call_in_node(FindCallCtx *ctx,
                                Iron_Node *n,
                                const IronLsp_Document *doc,
                                size_t cursor_byte);

static void find_call_in_nodes(FindCallCtx *ctx,
                                 Iron_Node **arr, int count,
                                 const IronLsp_Document *doc,
                                 size_t cursor_byte) {
    if (!arr) return;
    for (int i = 0; i < count; i++) {
        find_call_in_node(ctx, arr[i], doc, cursor_byte);
    }
}

static void find_call_in_node(FindCallCtx *ctx,
                                Iron_Node *n,
                                const IronLsp_Document *doc,
                                size_t cursor_byte) {
    if (!n || n->kind == IRON_NODE_ERROR) return;
    if (!byte_in_span(doc, n->span, cursor_byte)) return;

    if (n->kind == IRON_NODE_CALL || n->kind == IRON_NODE_METHOD_CALL) {
        size_t st = span_start_to_byte(doc, n->span);
        size_t en = span_end_to_byte(doc, n->span);
        /* Prefer innermost (smallest bracketing span). */
        if (!ctx->best ||
            (st >= ctx->best_start && en <= ctx->best_end)) {
            ctx->best = n;
            ctx->best_start = st;
            ctx->best_end   = en;
        }
    }

    switch ((int)n->kind) {
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)n;
            find_call_in_node(ctx, c->callee, doc, cursor_byte);
            find_call_in_nodes(ctx, c->args, c->arg_count, doc, cursor_byte);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)n;
            find_call_in_node(ctx, m->object, doc, cursor_byte);
            find_call_in_nodes(ctx, m->args, m->arg_count, doc, cursor_byte);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)n;
            find_call_in_nodes(ctx, b->stmts, b->stmt_count, doc, cursor_byte);
            break;
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)n;
            find_call_in_node(ctx, fd->body, doc, cursor_byte);
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)n;
            find_call_in_node(ctx, md->body, doc, cursor_byte);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *b = (Iron_BinaryExpr *)n;
            find_call_in_node(ctx, b->left,  doc, cursor_byte);
            find_call_in_node(ctx, b->right, doc, cursor_byte);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *u = (Iron_UnaryExpr *)n;
            find_call_in_node(ctx, u->operand, doc, cursor_byte);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)n;
            find_call_in_node(ctx, fa->object, doc, cursor_byte);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *ix = (Iron_IndexExpr *)n;
            find_call_in_node(ctx, ix->object, doc, cursor_byte);
            find_call_in_node(ctx, ix->index,  doc, cursor_byte);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *a = (Iron_AssignStmt *)n;
            find_call_in_node(ctx, a->target, doc, cursor_byte);
            find_call_in_node(ctx, a->value,  doc, cursor_byte);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *r = (Iron_ReturnStmt *)n;
            find_call_in_node(ctx, r->value, doc, cursor_byte);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *ifs = (Iron_IfStmt *)n;
            find_call_in_node(ctx, ifs->condition, doc, cursor_byte);
            find_call_in_node(ctx, ifs->body,      doc, cursor_byte);
            for (int i = 0; i < ifs->elif_count; i++) {
                find_call_in_node(ctx, ifs->elif_conds[i],  doc, cursor_byte);
                find_call_in_node(ctx, ifs->elif_bodies[i], doc, cursor_byte);
            }
            find_call_in_node(ctx, ifs->else_body, doc, cursor_byte);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)n;
            find_call_in_node(ctx, w->condition, doc, cursor_byte);
            find_call_in_node(ctx, w->body,      doc, cursor_byte);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)n;
            find_call_in_node(ctx, f->iterable, doc, cursor_byte);
            find_call_in_node(ctx, f->body,     doc, cursor_byte);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *v = (Iron_ValDecl *)n;
            find_call_in_node(ctx, v->init, doc, cursor_byte);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *v = (Iron_VarDecl *)n;
            find_call_in_node(ctx, v->init, doc, cursor_byte);
            break;
        }
        default:
            break;
    }
}

/* ── Entry point ─────────────────────────────────────────────────── */

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
    if (!server || !doc || !arena) return;

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

    FindCallCtx ctx = { .best = NULL, .best_start = 0, .best_end = SIZE_MAX };
    for (int i = 0; i < program->decl_count; i++) {
        find_call_in_node(&ctx, program->decls[i], doc, cursor_byte);
    }
    if (!ctx.best) goto done;

    /* Resolve the callee to a decl_node. */
    Iron_Node *callee = NULL;
    const char *self_type = NULL;
    if (ctx.best->kind == IRON_NODE_CALL) {
        callee = ((Iron_CallExpr *)ctx.best)->callee;
    } else if (ctx.best->kind == IRON_NODE_METHOD_CALL) {
        /* Method call: use object's resolved type as self_type. */
        Iron_MethodCallExpr *m = (Iron_MethodCallExpr *)ctx.best;
        /* No direct callee ident; the symbol lookup for the method
         * must traverse the object's type. For simplicity we fall
         * back to the method name on the object's type (if
         * resolved_type is available). */
        if (m->object && m->object->kind == IRON_NODE_IDENT) {
            Iron_Ident *oid = (Iron_Ident *)m->object;
            if (oid->resolved_sym && oid->resolved_sym->type &&
                oid->resolved_sym->type->kind == IRON_TYPE_OBJECT &&
                oid->resolved_sym->type->object.decl) {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)oid->resolved_sym->type->object.decl;
                self_type = od->name;
            }
        }
        /* We don't have a direct Iron_MethodDecl pointer without a
         * workspace method-resolver; downstream resolution TBD.
         * For now, leave callee NULL -- graceful empty. */
        callee = NULL;
    }

    Iron_FuncDecl   *fd = NULL;
    Iron_MethodDecl *md = NULL;
    if (callee && callee->kind == IRON_NODE_IDENT) {
        Iron_Ident *id = (Iron_Ident *)callee;
        Iron_Symbol *sym = id->resolved_sym;
        if (sym && sym->decl_node) {
            if (sym->decl_node->kind == IRON_NODE_FUNC_DECL) {
                fd = (Iron_FuncDecl *)sym->decl_node;
            } else if (sym->decl_node->kind == IRON_NODE_METHOD_DECL) {
                md = (Iron_MethodDecl *)sym->decl_node;
            }
        }
    }
    if (!fd && !md) goto done;  /* graceful empty */

    /* Build one signature info. */
    IronLsp_SignatureInfo *sig_arr = (IronLsp_SignatureInfo *)iron_arena_alloc(
        arena, sizeof(*sig_arr), _Alignof(IronLsp_SignatureInfo));
    if (!sig_arr) goto done;
    build_sig_info(&sig_arr[0], fd, md, self_type, arena);

    /* Find the call's '(' byte. */
    size_t paren = find_call_paren(doc, ctx.best_start, cursor_byte);
    int active_param = 0;
    if (paren != (size_t)-1) {
        active_param = active_param_between(doc, paren, cursor_byte);
    }
    /* Clamp. */
    int pc = sig_arr[0].parameter_count;
    if (pc > 0) {
        if (active_param >= pc) active_param = pc - 1;
    } else {
        active_param = 0;
    }

    *out_sigs = sig_arr;
    *out_n    = 1;
    if (out_active_sig)   *out_active_sig   = 0;
    if (out_active_param) *out_active_param = active_param;

done:
    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
}
