/* Phase 3 Plan 04 Task 02 (NAV-09, D-04) -- textDocument/hover facade.
 *
 * Initial stub (Task 01 build gate). Full implementation lands in
 * Task 02 and covers:
 *   - Signature derivation for every decl kind per D-04 mapping
 *   - Doc-comment concatenation (NAV-14 plumbing from Plan 01)
 *   - Active-diagnostic italic footer
 *   - 200-line / 8 KB markdown cap
 *   - Primitive-type name-only short-circuit
 */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/facade/nav/patch_lookup.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Cap constants (D-04) ────────────────────────────────────────── */

#define HOVER_MAX_LINES  200
#define HOVER_MAX_BYTES  8192

/* ── Small string-builder backed by an arena ─────────────────────── */

typedef struct {
    char        *buf;
    size_t       len;
    size_t       cap;
    Iron_Arena  *arena;
    bool         truncated;
} SB;

static void sb_init(SB *sb, Iron_Arena *arena) {
    sb->arena = arena;
    sb->cap   = 256;
    sb->len   = 0;
    sb->truncated = false;
    sb->buf = (char *)iron_arena_alloc(arena, sb->cap, 1);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_reserve(SB *sb, size_t need) {
    if (!sb->buf) return;
    if (sb->len + need + 1 <= sb->cap) return;
    size_t ncap = sb->cap;
    while (ncap < sb->len + need + 1) ncap *= 2;
    char *nb = (char *)iron_arena_alloc(sb->arena, ncap, 1);
    if (!nb) return;
    memcpy(nb, sb->buf, sb->len + 1);
    sb->buf = nb;
    sb->cap = ncap;
}

static void sb_append(SB *sb, const char *s) {
    if (!sb->buf || !s) return;
    size_t slen = strlen(s);
    if (sb->len + slen > HOVER_MAX_BYTES) {
        sb->truncated = true;
        size_t room = (sb->len < HOVER_MAX_BYTES) ? HOVER_MAX_BYTES - sb->len : 0;
        if (room == 0) return;
        slen = (slen < room) ? slen : room;
    }
    sb_reserve(sb, slen);
    if (!sb->buf) return;
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

/* ── Param rendering ──────────────────────────────────────────────── */

/* Map an Iron_TypeAnnotation AST node to a string. Favours
 * iron_type_to_string when a resolved type is present; otherwise
 * prints the annotation's own name/shape. */
static const char *render_type_ann(Iron_Node *n, Iron_Arena *arena) {
    if (!n) return "Void";
    if (n->kind == IRON_NODE_TYPE_ANNOTATION) {
        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)n;
        /* Minimal Path-style rendering: name + optional ? and array brackets. */
        SB tmp; sb_init(&tmp, arena);
        if (ta->is_array) sb_append(&tmp, "[");
        sb_append(&tmp, ta->name ? ta->name : "Unknown");
        if (ta->is_array) sb_append(&tmp, "]");
        if (ta->is_nullable) sb_append(&tmp, "?");
        return tmp.buf ? tmp.buf : "Unknown";
    }
    return "Unknown";
}

/* Render a function's parameter list (between parens, WITHOUT the
 * parens themselves): "a: Int, b: String". */
static void render_params(SB *sb, Iron_Node **params, int count,
                            Iron_Arena *arena) {
    for (int i = 0; i < count; i++) {
        if (i > 0) sb_append(sb, ", ");
        Iron_Node *p = params[i];
        if (!p || p->kind != IRON_NODE_PARAM) {
            sb_append(sb, "_");
            continue;
        }
        Iron_Param *pp = (Iron_Param *)p;
        sb_append(sb, pp->name ? pp->name : "_");
        sb_append(sb, ": ");
        sb_append(sb, render_type_ann(pp->type_ann, arena));
    }
}

/* ── Per-decl signature-line rendering (D-04) ────────────────────── */

static const char *signature_func(Iron_FuncDecl *fd, Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    /* Phase 10 D-11 / VIS-05: emit `pub ` prefix when func is publicly
     * visible. Order: pub -> readonly|pure -> func (Phase 9 D-10 lock).
     * Predicate normalises Iron_FuncDecl.is_private (v2 inverse) into a
     * positive boolean via the LSP-only adapter at visibility.c. */
    if (ilsp_vis_is_public((const Iron_Node *)fd)) sb_append(&sb, "pub ");
    /* Phase 9 AST-06: modifier prefix on hover signature line. Locked
     * order per CONTEXT.md D-10: readonly|pure -> func. Mutual exclusion
     * of readonly + pure is enforced by the parser at
     * src/parser/parser.c:3162-3180; both fields cannot be true at once,
     * so the order between them is academic for emission. */
    if (fd->is_readonly) sb_append(&sb, "readonly ");
    if (fd->is_pure)     sb_append(&sb, "pure ");
    sb_append(&sb, "func ");
    sb_append(&sb, fd->name ? fd->name : "_");
    sb_append(&sb, "(");
    render_params(&sb, fd->params, fd->param_count, arena);
    sb_append(&sb, ")");
    if (fd->return_type) {
        sb_append(&sb, " -> ");
        sb_append(&sb, render_type_ann(fd->return_type, arena));
    }
    return sb.buf ? sb.buf : "";
}

static const char *signature_method(Iron_MethodDecl *md, Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    /* Phase 9 AST-06: init form precedes any tier modifier per parser
     * rules (src/parser/parser.c:3232-3247). Anonymous init: `init(...)`.
     * Named init: `init <name>(...)`. is_init excludes pub/readonly/pure
     * by parser grammar so the modifier prefix is emitted only on the
     * regular method form. */
    if (md->is_init) {
        sb_append(&sb, "init");
        if (md->init_name) {
            sb_append(&sb, " ");
            sb_append(&sb, md->init_name);
        }
        sb_append(&sb, "(");
        render_params(&sb, md->params, md->param_count, arena);
        sb_append(&sb, ")");
        if (md->return_type) {
            sb_append(&sb, " -> ");
            sb_append(&sb, render_type_ann(md->return_type, arena));
        }
        return sb.buf ? sb.buf : "";
    }
    /* Phase 10 D-11 / VIS-05: emit `pub ` prefix when regular method
     * (NOT init) is publicly visible. Order: pub -> readonly|pure ->
     * func Type.method (Phase 9 D-10 lock). is_init form is short-
     * circuited above per Pitfall 7 -- `pub init` is grammar-rejected
     * by the parser (src/parser/parser.c:3232-3247), so the early-
     * return at the top of signature_method ensures we never emit
     * `pub init`. */
    if (ilsp_vis_is_public((const Iron_Node *)md)) sb_append(&sb, "pub ");
    /* Regular method form: pub -> readonly|pure -> func Type.method. */
    if (md->is_readonly) sb_append(&sb, "readonly ");
    if (md->is_pure)     sb_append(&sb, "pure ");
    sb_append(&sb, "func ");
    sb_append(&sb, md->type_name ? md->type_name : "_");
    sb_append(&sb, ".");
    sb_append(&sb, md->method_name ? md->method_name : "_");
    sb_append(&sb, "(");
    render_params(&sb, md->params, md->param_count, arena);
    sb_append(&sb, ")");
    if (md->return_type) {
        sb_append(&sb, " -> ");
        sb_append(&sb, render_type_ann(md->return_type, arena));
    }
    return sb.buf ? sb.buf : "";
}

static const char *signature_object(Iron_ObjectDecl *od, Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    /* Phase 10 D-11 / VIS-05: emit `pub ` prefix when object is publicly
     * visible. Order: pub -> patch -> object (Phase 8 F5 grammar lock).
     * Per RESEARCH Conflict 3, Iron_ObjectDecl has no is_private bit;
     * the predicate defaults-true and all objects render `pub`. This
     * satisfies REQUIREMENTS.md VIS-05 ("Hover displays pub modifier
     * explicitly when present"): every object IS publicly visible (the
     * language cannot represent a private object today, since the
     * parser drops `private` on top-level decls per parser.c:4047). */
    if (ilsp_vis_is_public((const Iron_Node *)od)) sb_append(&sb, "pub ");
    /* Phase 9 AST-06: emit `patch ` prefix for patch decls so hover on
     * `patch object Int { ... }` does not silently render as a regular
     * `object Int` declaration. Phase 11 PATCH-02 will surface the
     * target_type_name relationship as a feature. */
    if (od->is_patch) sb_append(&sb, "patch ");
    sb_append(&sb, "object ");
    sb_append(&sb, od->name ? od->name : "_");
    if (od->extends_name) {
        sb_append(&sb, " extends ");
        sb_append(&sb, od->extends_name);
    }
    if (od->implements_count > 0) {
        sb_append(&sb, " implements ");
        for (int i = 0; i < od->implements_count; i++) {
            if (i > 0) sb_append(&sb, ", ");
            sb_append(&sb, od->implements_names[i] ? od->implements_names[i] : "_");
        }
    }
    return sb.buf ? sb.buf : "";
}

static const char *signature_interface(Iron_InterfaceDecl *ifd,
                                          Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    sb_append(&sb, "interface ");
    sb_append(&sb, ifd->name ? ifd->name : "_");
    sb_append(&sb, " {\n");
    int cap = 20;
    int shown = ifd->method_count < cap ? ifd->method_count : cap;
    for (int i = 0; i < shown; i++) {
        Iron_Node *m = ifd->method_sigs[i];
        if (!m) continue;
        sb_append(&sb, "  ");
        if (m->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)m;
            sb_append(&sb, md->method_name ? md->method_name : "_");
            sb_append(&sb, "(");
            render_params(&sb, md->params, md->param_count, arena);
            sb_append(&sb, ")");
            if (md->return_type) {
                sb_append(&sb, " -> ");
                sb_append(&sb, render_type_ann(md->return_type, arena));
            }
        } else if (m->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)m;
            sb_append(&sb, fd->name ? fd->name : "_");
            sb_append(&sb, "(");
            render_params(&sb, fd->params, fd->param_count, arena);
            sb_append(&sb, ")");
            if (fd->return_type) {
                sb_append(&sb, " -> ");
                sb_append(&sb, render_type_ann(fd->return_type, arena));
            }
        }
        sb_append(&sb, ";\n");
    }
    if (ifd->method_count > cap) {
        char line[64];
        snprintf(line, sizeof(line), "  … %d more\n", ifd->method_count - cap);
        sb_append(&sb, line);
    }
    sb_append(&sb, "}");
    return sb.buf ? sb.buf : "";
}

static const char *signature_enum(Iron_EnumDecl *ed, Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    sb_append(&sb, "enum ");
    sb_append(&sb, ed->name ? ed->name : "_");
    sb_append(&sb, " {\n");
    int cap = 30;
    int shown = ed->variant_count < cap ? ed->variant_count : cap;
    for (int i = 0; i < shown; i++) {
        Iron_Node *v = ed->variants[i];
        if (!v || v->kind != IRON_NODE_ENUM_VARIANT) continue;
        Iron_EnumVariant *ev = (Iron_EnumVariant *)v;
        sb_append(&sb, "  ");
        sb_append(&sb, ev->name ? ev->name : "_");
        if (ev->payload_count > 0) {
            sb_append(&sb, "(");
            for (int j = 0; j < ev->payload_count; j++) {
                if (j > 0) sb_append(&sb, ", ");
                sb_append(&sb, render_type_ann(ev->payload_type_anns[j], arena));
            }
            sb_append(&sb, ")");
        }
        sb_append(&sb, ",\n");
    }
    if (ed->variant_count > cap) {
        char line[64];
        snprintf(line, sizeof(line), "  … %d more\n", ed->variant_count - cap);
        sb_append(&sb, line);
    }
    sb_append(&sb, "}");
    return sb.buf ? sb.buf : "";
}

static const char *signature_val(Iron_ValDecl *vd, Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    sb_append(&sb, "val ");
    sb_append(&sb, vd->name ? vd->name : "_");
    sb_append(&sb, ": ");
    sb_append(&sb, render_type_ann(vd->type_ann, arena));
    return sb.buf ? sb.buf : "";
}

static const char *signature_var(Iron_VarDecl *vd, Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    sb_append(&sb, "var ");
    sb_append(&sb, vd->name ? vd->name : "_");
    sb_append(&sb, ": ");
    sb_append(&sb, render_type_ann(vd->type_ann, arena));
    return sb.buf ? sb.buf : "";
}

static const char *signature_field(Iron_Field *fd,
                                     const char *owner_name,
                                     Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    /* Phase 9 AST-06: emit `pub ` prefix when is_pub is true so hover
     * on `pub var health: Int` no longer hides the visibility modifier.
     * Phase 10 VIS-05 will turn this into a feature with semantic
     * meaning. */
    if (fd->is_pub) sb_append(&sb, "pub ");
    if (owner_name) {
        sb_append(&sb, owner_name);
        sb_append(&sb, ".");
    }
    sb_append(&sb, fd->name ? fd->name : "_");
    sb_append(&sb, ": ");
    sb_append(&sb, render_type_ann(fd->type_ann, arena));
    return sb.buf ? sb.buf : "";
}

static const char *signature_enum_variant(Iron_EnumVariant *ev,
                                             const char *owner_name,
                                             Iron_Arena *arena) {
    SB sb; sb_init(&sb, arena);
    if (owner_name) {
        sb_append(&sb, owner_name);
        sb_append(&sb, ".");
    }
    sb_append(&sb, ev->name ? ev->name : "_");
    if (ev->payload_count > 0) {
        sb_append(&sb, "(");
        for (int j = 0; j < ev->payload_count; j++) {
            if (j > 0) sb_append(&sb, ", ");
            sb_append(&sb, render_type_ann(ev->payload_type_anns[j], arena));
        }
        sb_append(&sb, ")");
    }
    return sb.buf ? sb.buf : "";
}

/* ── Owner lookup (for Field and EnumVariant container name) ─────── */

static const char *find_field_owner(const Iron_Program *program,
                                      const Iron_Node    *decl) {
    if (!program || !decl) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *o = (Iron_ObjectDecl *)d;
            for (int j = 0; j < o->field_count; j++) {
                if (o->fields[j] == decl) return o->name;
            }
        } else if (d->kind == IRON_NODE_ENUM_DECL) {
            Iron_EnumDecl *e = (Iron_EnumDecl *)d;
            for (int j = 0; j < e->variant_count; j++) {
                if (e->variants[j] == decl) return e->name;
            }
        }
    }
    return NULL;
}

/* ── Markdown assembly (D-04 ordering) ────────────────────────────── */

/* Cap buf to HOVER_MAX_LINES lines. Mutates *buf in-place, appending
 * "\n… <N more lines>\n" if truncated. */
static const char *cap_lines(const char *in, Iron_Arena *arena) {
    if (!in) return in;
    int line_count = 0;
    for (const char *p = in; *p; p++) if (*p == '\n') line_count++;
    if (line_count <= HOVER_MAX_LINES) return in;
    /* Find the HOVER_MAX_LINES-th '\n'. */
    int seen = 0;
    const char *cut = in;
    for (const char *p = in; *p; p++) {
        if (*p == '\n') {
            seen++;
            if (seen == HOVER_MAX_LINES) { cut = p + 1; break; }
        }
    }
    size_t kept = (size_t)(cut - in);
    SB sb; sb_init(&sb, arena);
    sb_reserve(&sb, kept + 64);
    if (!sb.buf) return in;
    memcpy(sb.buf, in, kept);
    sb.len = kept;
    sb.buf[sb.len] = '\0';
    char extra[64];
    snprintf(extra, sizeof(extra), "… %d more lines\n", line_count - HOVER_MAX_LINES);
    sb_append(&sb, extra);
    return sb.buf;
}

static bool is_primitive_decl_like(const Iron_Symbol *sym) {
    /* Iron primitives are registered as IRON_SYM_TYPE with a primitive
     * Iron_Type kind. If kind == IRON_SYM_TYPE and the type is a
     * primitive (or the decl_node is NULL), treat as primitive. */
    if (!sym) return false;
    if (sym->sym_kind != IRON_SYM_TYPE) return false;
    if (!sym->type) return !sym->decl_node;
    switch ((int)sym->type->kind) {
        case IRON_TYPE_INT:
        case IRON_TYPE_INT8:  case IRON_TYPE_INT16:
        case IRON_TYPE_INT32: case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:
        case IRON_TYPE_UINT8: case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32:case IRON_TYPE_UINT64:
        case IRON_TYPE_FLOAT: case IRON_TYPE_FLOAT32: case IRON_TYPE_FLOAT64:
        case IRON_TYPE_BOOL:  case IRON_TYPE_STRING:
        case IRON_TYPE_VOID:
            return true;
        default:
            return false;
    }
}

/* Entry point: populate *out with a hover result for the cursor. */
void ilsp_facade_hover(struct IronLsp_Server   *server,
                        struct IronLsp_Document *doc,
                        IronLsp_Position         pos,
                        _Atomic bool            *cancel,
                        Iron_Arena              *arena,
                        IronLsp_HoverResult     *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
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

    Iron_Node *node = ilsp_nav_node_at(doc, program, pos, enc);
    if (!node) goto done;

    const Iron_Symbol *sym = NULL;
    Iron_Node *decl = NULL;
    Iron_Span target_span = node->span;
    if (node->kind == IRON_NODE_IDENT) {
        Iron_Ident *id = (Iron_Ident *)node;
        sym = id->resolved_sym;
        if (sym) decl = sym->decl_node;
    } else {
        /* Cursor on a decl itself -- hover shows its own signature. */
        decl = node;
    }

    /* Primitive short-circuit: name-only, no doc comment, no diags. */
    if (sym && is_primitive_decl_like(sym)) {
        SB sb; sb_init(&sb, arena);
        sb_append(&sb, "```iron\n");
        sb_append(&sb, sym->name ? sym->name : "");
        sb_append(&sb, "\n```");
        out->markdown = sb.buf;
        out->range = ilsp_span_to_lsp_range(target_span, doc, enc);
        out->has_range = true;
        goto done;
    }

    if (!decl) goto done;

    /* Derive signature + doc_comment based on decl kind. */
    const char *sig = NULL;
    const char *dc = NULL;
    switch ((int)decl->kind) {
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
            sig = signature_func(fd, arena);
            dc  = fd->doc_comment;
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
            sig = signature_method(md, arena);
            dc  = md->doc_comment;
            break;
        }
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
            sig = signature_object(od, arena);
            dc  = od->doc_comment;
            break;
        }
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *ifd = (Iron_InterfaceDecl *)decl;
            sig = signature_interface(ifd, arena);
            dc  = ifd->doc_comment;
            break;
        }
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *ed = (Iron_EnumDecl *)decl;
            sig = signature_enum(ed, arena);
            dc  = ed->doc_comment;
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)decl;
            sig = signature_val(vd, arena);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)decl;
            sig = signature_var(vd, arena);
            break;
        }
        case IRON_NODE_FIELD: {
            Iron_Field *fd = (Iron_Field *)decl;
            const char *owner = find_field_owner(program, decl);
            sig = signature_field(fd, owner, arena);
            dc  = fd->doc_comment;
            break;
        }
        case IRON_NODE_ENUM_VARIANT: {
            Iron_EnumVariant *ev = (Iron_EnumVariant *)decl;
            const char *owner = find_field_owner(program, decl);
            sig = signature_enum_variant(ev, owner, arena);
            dc  = ev->doc_comment;
            break;
        }
        case IRON_NODE_IMPORT_DECL: {
            Iron_ImportDecl *imp = (Iron_ImportDecl *)decl;
            SB sb; sb_init(&sb, arena);
            sb_append(&sb, "import ");
            sb_append(&sb, imp->path ? imp->path : "");
            if (imp->alias) {
                sb_append(&sb, " as ");
                sb_append(&sb, imp->alias);
            }
            sig = sb.buf;
            dc  = imp->doc_comment;
            break;
        }
        default:
            /* Unsupported decl kind; degrade gracefully. */
            goto done;
    }
    if (!sig) goto done;

    /* Build the final markdown per D-04 ordering. */
    SB md; sb_init(&md, arena);

    /* PATCH-04 (Plan 11-03): prepend italic context line for patch-contributed
     * methods. Predicate-gated: ilsp_patch_enclosing_for_method returns NULL
     * for non-patch methods, so native hover renders unchanged (D-13). The
     * italic line precedes the existing fenced code block so editors that
     * render markdown show "_From `patch object T { … }` in <module>_" above
     * the existing signature. The same line is suppressed when the cursor is
     * on a non-method decl (function, field, object, enum, etc.) because the
     * predicate-gated condition checks decl->kind == IRON_NODE_METHOD_DECL.
     * Phase 9 AST-06 `patch object T` prefix on object-level hover is
     * UNTOUCHED (lives in signature_object). */
    if (decl && decl->kind == IRON_NODE_METHOD_DECL) {
        Iron_MethodDecl *md_node = (Iron_MethodDecl *)decl;
        Iron_ObjectDecl *patch_od =
            ilsp_patch_enclosing_for_method(
                program,
                md_node,
                server ? server->workspace_index : NULL);
        if (patch_od && patch_od->span.filename) {
            sb_append(&md, "_From `patch object ");
            sb_append(&md, patch_od->target_type_name
                           ? patch_od->target_type_name : "_");
            sb_append(&md, " { … }` in ");
            sb_append(&md, patch_od->span.filename);
            sb_append(&md, "_\n\n");
        }
    }

    sb_append(&md, "```iron\n");
    sb_append(&md, sig);
    sb_append(&md, "\n```");
    if (dc && *dc) {
        sb_append(&md, "\n\n");
        sb_append(&md, dc);
    }

    /* Active-diag footer (D-04 step 4): scan diags from the
     * compile-for-nav call. Use the first diag whose span covers the
     * target_span's start. */
    for (int i = 0; diags.items && i < diags.count; i++) {
        Iron_Diagnostic *dg = &diags.items[i];
        Iron_Span ds = dg->span;
        if (ds.line > target_span.line) continue;
        if (ds.end_line < target_span.line) continue;
        if (ds.line == target_span.line && ds.col > target_span.col) continue;
        if (ds.end_line == target_span.line && ds.end_col < target_span.col) continue;
        sb_append(&md, "\n\n_");
        sb_append(&md, dg->message ? dg->message : "");
        char code[32];
        snprintf(code, sizeof(code), " (iron E%04d)", dg->code);
        sb_append(&md, code);
        sb_append(&md, "_");
        break;
    }

    out->markdown = cap_lines(md.buf, arena);
    out->range = ilsp_span_to_lsp_range(target_span, doc, enc);
    out->has_range = true;

done:
    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
}
