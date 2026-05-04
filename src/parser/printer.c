#include "parser/printer.h"
#include "parser/ast.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "fmt/options.h"   /* IronFmtOptions */
#include "lexer/lexer.h"   /* Iron_TokenKind for operator names */
#include "vendor/stb_ds.h" /* Phase 9 Plan 09-02: shput/shget/arrput for the
                            * method-by-type-name index used by the
                            * OBJECT_DECL method-merge path (D-10 Approach 1). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Method-by-type-name index (Phase 9 Plan 09-02 D-10) ─────────────────── */

/* Bucket value: a malloc-grown stb_ds dynamic array of receiver-form methods
 * owned by a single type. arrput'd; arrfree'd at teardown. */
typedef struct {
    Iron_MethodDecl **methods;
} MethodOwnerArr;

/* stb_ds shmap entry shape: shput(t, k, v) writes (t)[i].key=k and
 * (t)[i].value=v (see stb_ds.h:563-566), so the array element type must
 * have both `.key` and `.value` fields. Map type is `MethodIndexEntry *`
 * dynamic; iterate via shlen / map[i].value.methods. */
typedef struct {
    char           *key;     /* type name; stb_ds owns the realloc'd slot */
    MethodOwnerArr  value;
} MethodIndexEntry;

/* ── Printer context ─────────────────────────────────────────────────────── */

typedef struct {
    Iron_StrBuf          *sb;
    int                   indent_level;
    const IronFmtOptions *opts;   /* Phase 5 Plan 05-01: NULL means defaults */
    /* Phase 9 Plan 09-02 D-10 Approach 1: method-by-type-name index built
     * once per iron_print_ast call. NULL when print_node is invoked
     * recursively without a top-level Iron_Program (e.g., printing a single
     * AST subtree); in that case the merge logic short-circuits and methods
     * print at top level via the standalone METHOD_DECL arm. void* keeps
     * MethodOwnerArr private to printer.c. */
    void                 *method_index;  /* (MethodIndexEntry *) shmap or NULL */
} PrintCtx;

/* ── Method index: build / lookup / free ─────────────────────────────────── */

/* Build a method-by-type-name index from program->decls[] including only
 * methods with is_receiver_form==true. The bit-explicit filter is the
 * Pitfall 3 mitigation: classic v2 `func Type.method(...)` decls leave
 * is_receiver_form=false (parser.c:3040-3041 and the v2 receiver-form
 * branch elsewhere) and therefore stay at top level on print, preserving
 * v2 byte output verbatim. v3 method-in-block / patch / init decls are
 * hoisted to program->decls[] by the parser (parser.c:3331-3460,
 * :3208-3335) and carry is_receiver_form=true; those are the only ones
 * pulled into their owning object body during printing. */
static MethodIndexEntry *method_index_build(Iron_Program *prog) {
    if (!prog) return NULL;
    MethodIndexEntry *map = NULL;
    /* Don't use sh_new_arena — the printer's arena is the OUTPUT string
     * arena and must not absorb transient bookkeeping. Plain shput uses
     * stb_ds default malloc; freed in method_index_free below. */
    int any = 0;
    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->is_receiver_form) continue;
        if (!md->type_name) continue;
        any = 1;
        int idx = shgeti(map, md->type_name);
        if (idx < 0) {
            MethodOwnerArr empty = { .methods = NULL };
            shput(map, md->type_name, empty);
            idx = shgeti(map, md->type_name);
        }
        arrput(map[idx].value.methods, md);
    }
    if (!any) {
        if (map) shfree(map);
        return NULL;
    }
    return map;
}

/* Lookup the method bucket for a given type name. Returns NULL if no
 * receiver-form methods own that type. Safe on map==NULL. */
static MethodOwnerArr *method_index_lookup(MethodIndexEntry *map,
                                             const char *type_name) {
    if (!map || !type_name) return NULL;
    int idx = shgeti(map, type_name);
    if (idx < 0) return NULL;
    return &map[idx].value;
}

/* Test whether a given Iron_MethodDecl is in the suppression set — i.e.,
 * will be re-emitted inside the body of its owning Iron_ObjectDecl during
 * the OBJECT_DECL print and therefore must NOT print at top level too. */
static int method_index_should_suppress(MethodIndexEntry *map,
                                          Iron_MethodDecl *md) {
    if (!map || !md) return 0;
    if (!md->is_receiver_form || !md->type_name) return 0;
    return method_index_lookup(map, md->type_name) != NULL;
}

/* Free the index: each bucket's methods array (arrfree) plus the shmap
 * itself (shfree). Safe on NULL. */
static void method_index_free(MethodIndexEntry *map) {
    if (!map) return;
    for (ptrdiff_t i = 0; i < shlen(map); i++) {
        arrfree(map[i].value.methods);
    }
    shfree(map);
}

/* Forward decl (defined alongside print_node below). */
static void print_method_in_block(PrintCtx *ctx, Iron_MethodDecl *m);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Phase 5 Plan 05-01 (D-01, FMT-05): honor opts->use_tabs and
 * opts->indent_width. NULL opts or opts->indent_width <= 0 fall back
 * to 2 spaces -- byte-identical to pre-Phase-5 behavior (RESEARCH A1). */
static void print_indent(PrintCtx *ctx) {
    if (ctx->opts && ctx->opts->use_tabs) {
        for (int i = 0; i < ctx->indent_level; i++) {
            iron_strbuf_appendf(ctx->sb, "\t");
        }
    } else {
        int width = (ctx->opts && ctx->opts->indent_width > 0)
                    ? ctx->opts->indent_width
                    : 2;    /* preserve pre-Phase-5 default */
        for (int i = 0; i < ctx->indent_level; i++) {
            for (int j = 0; j < width; j++) iron_strbuf_appendf(ctx->sb, " ");
        }
    }
}

/* Operator token kind → string */
static const char *op_str(Iron_OpKind op) {
    switch ((int)op) {
        case IRON_TOK_PLUS:          return "+";
        case IRON_TOK_MINUS:         return "-";
        case IRON_TOK_STAR:          return "*";
        case IRON_TOK_SLASH:         return "/";
        case IRON_TOK_PERCENT:       return "%";
        case IRON_TOK_EQUALS:        return "==";
        case IRON_TOK_NOT_EQUALS:    return "!=";
        case IRON_TOK_LESS:          return "<";
        case IRON_TOK_GREATER:       return ">";
        case IRON_TOK_LESS_EQ:       return "<=";
        case IRON_TOK_GREATER_EQ:    return ">=";
        case IRON_TOK_AND:           return "and";
        case IRON_TOK_OR:            return "or";
        case IRON_TOK_ASSIGN:        return "=";
        case IRON_TOK_PLUS_ASSIGN:   return "+=";
        case IRON_TOK_MINUS_ASSIGN:  return "-=";
        case IRON_TOK_STAR_ASSIGN:   return "*=";
        case IRON_TOK_SLASH_ASSIGN:  return "/=";
        /* Bitwise operators (AUDIT-02 #1 fix — previously silently mapped to "?") */
        case IRON_TOK_AMP:           return "&";
        case IRON_TOK_PIPE:          return "|";
        case IRON_TOK_CARET:         return "^";
        case IRON_TOK_TILDE:         return "~";
        case IRON_TOK_SHL:           return "<<";
        case IRON_TOK_SHR:           return ">>";
        case IRON_TOK_SHL_ASSIGN:    return "<<=";
        case IRON_TOK_SHR_ASSIGN:    return ">>=";
        case IRON_TOK_AMP_ASSIGN:    return "&=";
        case IRON_TOK_PIPE_ASSIGN:   return "|=";
        case IRON_TOK_CARET_ASSIGN:  return "^=";
        /* -Wswitch-enum opt-out: op_str is operator-only; the ~60 non-operator
         * Iron_TokenKind values (literals, keywords, punctuation, errors)
         * are not valid Iron_OpKind values and intentionally fall through to
         * "?" so malformed AST round-trips print something rather than crash. */
        default:                     return "?";
    }
}

/* Forward-declare so print_node can be called recursively */
static void print_node(PrintCtx *ctx, Iron_Node *node);

/* Print a type annotation */
static void print_type_ann(PrintCtx *ctx, Iron_Node *node) {
    if (!node) return;
    if (node->kind == IRON_NODE_TYPE_ANNOTATION) {
        Iron_TypeAnnotation *t = (Iron_TypeAnnotation *)node;
        if (t->is_array) {
            iron_strbuf_appendf(ctx->sb, "[%s", t->name);
            if (t->array_size) {
                iron_strbuf_appendf(ctx->sb, "; ");
                print_node(ctx, t->array_size);
            }
            iron_strbuf_appendf(ctx->sb, "]");
        } else {
            iron_strbuf_appendf(ctx->sb, "%s", t->name);
            if (t->is_nullable) {
                iron_strbuf_appendf(ctx->sb, "?");
            }
            if (t->generic_arg_count > 0) {
                iron_strbuf_appendf(ctx->sb, "[");
                for (int i = 0; i < t->generic_arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_type_ann(ctx, t->generic_args[i]);
                }
                iron_strbuf_appendf(ctx->sb, "]");
            }
        }
    } else {
        /* Fallback: print as identifier */
        print_node(ctx, node);
    }
}

/* Print a parameter */
static void print_param(PrintCtx *ctx, Iron_Node *node) {
    if (!node || node->kind != IRON_NODE_PARAM) return;
    Iron_Param *p = (Iron_Param *)node;
    if (p->is_var) iron_strbuf_appendf(ctx->sb, "var ");
    iron_strbuf_appendf(ctx->sb, "%s", p->name);
    if (p->type_ann) {
        iron_strbuf_appendf(ctx->sb, ": ");
        print_type_ann(ctx, p->type_ann);
    }
}

/* Print a comma-separated parameter list */
static void print_params(PrintCtx *ctx, Iron_Node **params, int count) {
    iron_strbuf_appendf(ctx->sb, "(");
    for (int i = 0; i < count; i++) {
        if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
        print_param(ctx, params[i]);
    }
    iron_strbuf_appendf(ctx->sb, ")");
}

/* Print optional generic params: [T, U] */
static void print_generic_params(PrintCtx *ctx, Iron_Node **gps, int count) {
    if (count == 0) return;
    iron_strbuf_appendf(ctx->sb, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
        if (gps[i]->kind == IRON_NODE_IDENT) {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_Ident *)gps[i])->name);
        } else {
            print_type_ann(ctx, gps[i]);
        }
    }
    iron_strbuf_appendf(ctx->sb, "]");
}

/* Print a block with proper indentation */
static void print_block(PrintCtx *ctx, Iron_Node *node) {
    if (!node) return;
    if (node->kind != IRON_NODE_BLOCK) {
        print_node(ctx, node);
        return;
    }
    Iron_Block *blk = (Iron_Block *)node;
    iron_strbuf_appendf(ctx->sb, "{\n");
    ctx->indent_level++;
    for (int i = 0; i < blk->stmt_count; i++) {
        print_indent(ctx);
        print_node(ctx, blk->stmts[i]);
        iron_strbuf_appendf(ctx->sb, "\n");
    }
    ctx->indent_level--;
    print_indent(ctx);
    iron_strbuf_appendf(ctx->sb, "}");
}

/* Print a call argument list */
static void print_args(PrintCtx *ctx, Iron_Node **args, int count) {
    iron_strbuf_appendf(ctx->sb, "(");
    for (int i = 0; i < count; i++) {
        if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
        print_node(ctx, args[i]);
    }
    iron_strbuf_appendf(ctx->sb, ")");
}

/* Phase 9 Plan 09-02 D-10 Approach 1: print a single Iron_MethodDecl using
 * the v3 in-block source shape. Reached only for methods that the
 * OBJECT_DECL printer is re-merging into the body of their owning object
 * (i.e., methods that the parser hoisted out of an object/patch body via
 * the `is_receiver_form==true` path at parser.c:3208-3460). The
 * synthesized self param is at index 0 and is skipped from the rendered
 * param list to match the source shape (`func name(args)` not
 * `func name(self: T, args)`). */
static void print_method_in_block(PrintCtx *ctx, Iron_MethodDecl *m) {
    if (m->is_private) iron_strbuf_appendf(ctx->sb, "private ");
    /* Init form precedes any tier modifier; tier modifiers on init are
     * rejected at parse time (parser.c:3219-3228), so this branch never
     * combines init with readonly/pure. The init grammar produces method
     * declarations with method_name=="init" (anonymous) or
     * method_name==init_name (named); we render the source-side decl
     * shape `init` / `init <name>` followed by the param list. */
    if (m->is_init) {
        iron_strbuf_appendf(ctx->sb, "init");
        if (m->init_name) {
            iron_strbuf_appendf(ctx->sb, " %s", m->init_name);
        }
        /* Skip the parser-synthesized implicit self at index 0
         * (parser.c:3297). The remainder are explicit init params. */
        if (m->param_count > 0) {
            print_params(ctx, m->params + 1, m->param_count - 1);
        } else {
            iron_strbuf_appendf(ctx->sb, "()");
        }
        if (m->body) {
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, m->body);
        }
        return;
    }
    /* Phase 9 Plan 09-02 D-10: visibility-before-tier-before-func order.
     * Iron_MethodDecl has no is_pub field — the parser silently drops
     * `pub` on methods (parser.c:3444 / :4385). For v3 fixtures whose
     * source includes `pub` on methods (e.g., v3_init_anonymous_and_named's
     * `pub readonly func read()`), the printer cannot recover the literal
     * `pub` token after parse — but the parser's lossy treatment of `pub`
     * on methods is deliberate (methods default public in v2.2/v3.0) and
     * the round-trip stable form drops `pub` on methods. The fixed-point
     * property still holds because both p1 and p2 in
     * iron_print_ast(parse(iron_print_ast(parse(src)))) drop the same bits. */
    if (m->is_readonly) iron_strbuf_appendf(ctx->sb, "readonly ");
    if (m->is_pure)     iron_strbuf_appendf(ctx->sb, "pure ");
    iron_strbuf_appendf(ctx->sb, "func %s", m->method_name);
    print_generic_params(ctx, m->generic_params, m->generic_param_count);
    /* Skip implicit self at index 0; matches Phase 82 / 85 / 86 synthesis
     * sites at parser.c:3297, :3409. */
    if (m->param_count > 0) {
        print_params(ctx, m->params + 1, m->param_count - 1);
    } else {
        iron_strbuf_appendf(ctx->sb, "()");
    }
    if (m->return_type) {
        iron_strbuf_appendf(ctx->sb, " -> ");
        print_type_ann(ctx, m->return_type);
    }
    if (m->body) {
        iron_strbuf_appendf(ctx->sb, " ");
        print_block(ctx, m->body);
    }
}

/* ── Main node printer ───────────────────────────────────────────────────── */

static void print_node(PrintCtx *ctx, Iron_Node *node) {
    if (!node) return;

    switch (node->kind) {

        /* ── Program ── */
        case IRON_NODE_PROGRAM: {
            Iron_Program *n = (Iron_Program *)node;
            /* Phase 9 Plan 09-02 D-10 Approach 1: skip receiver-form methods
             * that have been re-emitted inside their owning object body. The
             * separator-on-non-first logic must observe the same skip so
             * surviving decls keep single-blank-line separation (Pitfall 3:
             * a stray blank line where the suppressed method used to be
             * would diverge from the v2 baseline). */
            int printed = 0;
            for (int i = 0; i < n->decl_count; i++) {
                Iron_Node *d = n->decls[i];
                if (d && d->kind == IRON_NODE_METHOD_DECL) {
                    if (method_index_should_suppress(
                            (MethodIndexEntry *)ctx->method_index,
                            (Iron_MethodDecl *)d)) {
                        continue;  /* re-emitted inside owning object body */
                    }
                }
                if (printed > 0) iron_strbuf_appendf(ctx->sb, "\n");
                print_node(ctx, d);
                iron_strbuf_appendf(ctx->sb, "\n");
                printed++;
            }
            break;
        }

        /* ── Top-level declarations ── */
        case IRON_NODE_IMPORT_DECL: {
            Iron_ImportDecl *n = (Iron_ImportDecl *)node;
            iron_strbuf_appendf(ctx->sb, "import %s", n->path);
            if (n->alias) iron_strbuf_appendf(ctx->sb, " as %s", n->alias);
            break;
        }

        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *n = (Iron_ObjectDecl *)node;
            /* Emit `pub` and/or `patch` prefixes in Iron source order:
             * `pub patch object T`. n->name and n->target_type_name
             * reference the same arena strdup for patches so we can
             * keep using n->name for the identifier. */
            if (n->is_pub)   iron_strbuf_appendf(ctx->sb, "pub ");
            if (n->is_patch) iron_strbuf_appendf(ctx->sb, "patch ");
            iron_strbuf_appendf(ctx->sb, "object %s", n->name);
            print_generic_params(ctx, n->generic_params, n->generic_param_count);
            if (n->extends_name) {
                iron_strbuf_appendf(ctx->sb, " extends %s", n->extends_name);
            }
            if (n->implements_count > 0) {
                /* Phase 9 Plan 09-02 D-10: classic `object T impl I` uses the
                 * IRON_TOK_IMPL keyword (parser.c:3117); patches use the
                 * contextual identifier `implements` (parser.c:4127-4129).
                 * Round-tripping requires emitting the matching token. */
                iron_strbuf_appendf(ctx->sb,
                                     n->is_patch ? " implements " : " impl ");
                for (int i = 0; i < n->implements_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    iron_strbuf_appendf(ctx->sb, "%s", n->implements_names[i]);
                }
            }
            iron_strbuf_appendf(ctx->sb, " {\n");
            ctx->indent_level++;
            for (int i = 0; i < n->field_count; i++) {
                print_indent(ctx);
                Iron_Field *f = (Iron_Field *)n->fields[i];
                /* Phase 9 Plan 09-02 D-10: pub prefix on field rendered
                 * inside an object body. Mirrors the standalone IRON_NODE_FIELD
                 * arm; this is the load-bearing path because that standalone
                 * arm is otherwise unreachable from iron_print_ast (fields
                 * only enter the printer through their owning object). */
                if (f->is_pub) iron_strbuf_appendf(ctx->sb, "pub ");
                iron_strbuf_appendf(ctx->sb, "%s %s", f->is_var ? "var" : "val", f->name);
                if (f->type_ann) {
                    iron_strbuf_appendf(ctx->sb, ": ");
                    print_type_ann(ctx, f->type_ann);
                }
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            /* Phase 9 Plan 09-02 D-10 Approach 1: re-merge receiver-form
             * methods into the object body. The key is target_type_name when
             * is_patch=true and name otherwise — but parser.c:4448 sets
             * target_type_name == name for patches, so n->name covers both.
             * The bit-explicit `is_receiver_form` filter at index-build time
             * keeps classic v2 `func Type.method(...)` decls (which leave
             * is_receiver_form=false) at top level — Pitfall 3 mitigation. */
            MethodIndexEntry *map = (MethodIndexEntry *)ctx->method_index;
            if (map) {
                const char *key = n->is_patch && n->target_type_name
                                   ? n->target_type_name : n->name;
                MethodOwnerArr *slot = method_index_lookup(map, key);
                if (slot) {
                    for (ptrdiff_t i = 0; i < arrlen(slot->methods); i++) {
                        print_indent(ctx);
                        print_method_in_block(ctx, slot->methods[i]);
                        iron_strbuf_appendf(ctx->sb, "\n");
                    }
                }
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *n = (Iron_InterfaceDecl *)node;
            iron_strbuf_appendf(ctx->sb, "interface %s {\n", n->name);
            ctx->indent_level++;
            for (int i = 0; i < n->method_count; i++) {
                print_indent(ctx);
                Iron_FuncDecl *sig = (Iron_FuncDecl *)n->method_sigs[i];
                iron_strbuf_appendf(ctx->sb, "func %s", sig->name);
                print_params(ctx, sig->params, sig->param_count);
                if (sig->return_type) {
                    iron_strbuf_appendf(ctx->sb, " -> ");
                    print_type_ann(ctx, sig->return_type);
                }
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *n = (Iron_EnumDecl *)node;
            if (n->is_pub) iron_strbuf_appendf(ctx->sb, "pub ");
            iron_strbuf_appendf(ctx->sb, "enum %s {\n", n->name);
            ctx->indent_level++;
            for (int i = 0; i < n->variant_count; i++) {
                print_indent(ctx);
                Iron_EnumVariant *ev = (Iron_EnumVariant *)n->variants[i];
                iron_strbuf_appendf(ctx->sb, "%s", ev->name);
                if (ev->payload_count > 0) {
                    iron_strbuf_appendf(ctx->sb, "(");
                    for (int j = 0; j < ev->payload_count; j++) {
                        if (j > 0) iron_strbuf_appendf(ctx->sb, ", ");
                        print_node(ctx, ev->payload_type_anns[j]);
                    }
                    iron_strbuf_appendf(ctx->sb, ")");
                }
                iron_strbuf_appendf(ctx->sb, ",\n");
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *n = (Iron_FuncDecl *)node;
            if (n->is_pub) iron_strbuf_appendf(ctx->sb, "pub ");
            if (n->is_private) iron_strbuf_appendf(ctx->sb, "private ");
            /* Phase 9 Plan 09-02 D-10: tier modifier prefix in locked order
             * (visibility-before-tier-before-func). Iron_FuncDecl carries
             * is_readonly / is_pure for interface method signatures only —
             * top-level func decls reject these tokens at parse time
             * (parser.c:4937-4955), so on a parsed-clean program these bits
             * are only ever true for nodes stored inside an Iron_InterfaceDecl
             * method_sigs array. is_pub is intentionally absent on
             * Iron_FuncDecl (parser silently drops `pub` on object-block
             * methods at parser.c:3444 / :4385 and rejects it on top-level
             * func decls at parser.c:4937). */
            if (n->is_readonly) iron_strbuf_appendf(ctx->sb, "readonly ");
            if (n->is_pure)     iron_strbuf_appendf(ctx->sb, "pure ");
            iron_strbuf_appendf(ctx->sb, "func %s", n->name);
            print_generic_params(ctx, n->generic_params, n->generic_param_count);
            print_params(ctx, n->params, n->param_count);
            if (n->return_type) {
                iron_strbuf_appendf(ctx->sb, " -> ");
                print_type_ann(ctx, n->return_type);
            }
            if (n->body) {
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, n->body);
            }
            break;
        }

        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *n = (Iron_MethodDecl *)node;
            if (n->is_pub) iron_strbuf_appendf(ctx->sb, "pub ");
            if (n->is_private) iron_strbuf_appendf(ctx->sb, "private ");
            /* Phase 9 Plan 09-02 D-10: tier modifier prefix on the standalone
             * (top-level) method print path. Locked order is
             * `[private] [readonly|pure] func ...`. Iron_MethodDecl has no
             * is_pub field — the parser silently drops `pub` on methods
             * (parser.c:3444 / :4385) because methods default public in
             * v2.2/v3.0. is_init / init_name are NOT printed on this
             * standalone path; the in-block init form `init [name](args)` is
             * emitted only via print_method_in_block (Task 3). On the
             * standalone path an init method (if any reaches it before the
             * Task 3 method-merge lands) falls through to the legacy
             * `func Type.init(...)` shape, which is acceptable as a transient
             * since v3 fixtures with init methods are exercised exclusively
             * by the parity test that the merge satisfies. */
            if (n->is_readonly) iron_strbuf_appendf(ctx->sb, "readonly ");
            if (n->is_pure)     iron_strbuf_appendf(ctx->sb, "pure ");
            if (n->is_receiver_form && n->param_count > 0) {
                /* Receiver form: `func (recv: Type) method[G](rest...)`.
                 * By parser invariant, params[0] is the receiver. */
                Iron_Param *recv = (Iron_Param *)n->params[0];
                iron_strbuf_appendf(ctx->sb, "func (");
                /* Phase 79 MUT-01: `mut` is mutually exclusive with `var` at
                 * parse time, so only one of these two prints will fire. Print
                 * `mut` first to match the canonical Iron source order
                 * `func (mut t: T)`. */
                if (recv->is_mut_receiver) iron_strbuf_appendf(ctx->sb, "mut ");
                if (recv->is_var) iron_strbuf_appendf(ctx->sb, "var ");
                iron_strbuf_appendf(ctx->sb, "%s", recv->name);
                if (recv->type_ann) {
                    iron_strbuf_appendf(ctx->sb, ": ");
                    print_type_ann(ctx, recv->type_ann);
                }
                iron_strbuf_appendf(ctx->sb, ") %s", n->method_name);
                print_generic_params(ctx, n->generic_params, n->generic_param_count);
                print_params(ctx, n->params + 1, n->param_count - 1);
            } else {
                iron_strbuf_appendf(ctx->sb, "func %s.%s", n->type_name, n->method_name);
                print_generic_params(ctx, n->generic_params, n->generic_param_count);
                print_params(ctx, n->params, n->param_count);
            }
            if (n->return_type) {
                iron_strbuf_appendf(ctx->sb, " -> ");
                print_type_ann(ctx, n->return_type);
            }
            if (n->body) {
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, n->body);
            }
            break;
        }

        /* ── Statements ── */
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *n = (Iron_ValDecl *)node;
            iron_strbuf_appendf(ctx->sb, "val %s", n->name);
            if (n->type_ann) {
                iron_strbuf_appendf(ctx->sb, ": ");
                print_type_ann(ctx, n->type_ann);
            }
            if (n->init) {
                iron_strbuf_appendf(ctx->sb, " = ");
                print_node(ctx, n->init);
            }
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *n = (Iron_VarDecl *)node;
            iron_strbuf_appendf(ctx->sb, "var %s", n->name);
            if (n->type_ann) {
                iron_strbuf_appendf(ctx->sb, ": ");
                print_type_ann(ctx, n->type_ann);
            }
            if (n->init) {
                iron_strbuf_appendf(ctx->sb, " = ");
                print_node(ctx, n->init);
            }
            break;
        }

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *n = (Iron_AssignStmt *)node;
            print_node(ctx, n->target);
            iron_strbuf_appendf(ctx->sb, " %s ", op_str(n->op));
            print_node(ctx, n->value);
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *n = (Iron_ReturnStmt *)node;
            if (n->value) {
                iron_strbuf_appendf(ctx->sb, "return ");
                print_node(ctx, n->value);
            } else {
                iron_strbuf_appendf(ctx->sb, "return");
            }
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *n = (Iron_IfStmt *)node;
            iron_strbuf_appendf(ctx->sb, "if ");
            print_node(ctx, n->condition);
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            for (int i = 0; i < n->elif_count; i++) {
                iron_strbuf_appendf(ctx->sb, " elif ");
                print_node(ctx, n->elif_conds[i]);
                iron_strbuf_appendf(ctx->sb, " ");
                print_block(ctx, n->elif_bodies[i]);
            }
            if (n->else_body) {
                iron_strbuf_appendf(ctx->sb, " else ");
                print_block(ctx, n->else_body);
            }
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *n = (Iron_WhileStmt *)node;
            iron_strbuf_appendf(ctx->sb, "while ");
            print_node(ctx, n->condition);
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *n = (Iron_ForStmt *)node;
            iron_strbuf_appendf(ctx->sb, "for %s in ", n->var_name);
            print_node(ctx, n->iterable);
            if (n->is_parallel) {
                iron_strbuf_appendf(ctx->sb, " parallel");
                if (n->pool_expr) {
                    iron_strbuf_appendf(ctx->sb, "(");
                    print_node(ctx, n->pool_expr);
                    iron_strbuf_appendf(ctx->sb, ")");
                }
            }
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_MATCH: {
            Iron_MatchStmt *n = (Iron_MatchStmt *)node;
            iron_strbuf_appendf(ctx->sb, "match ");
            print_node(ctx, n->subject);
            iron_strbuf_appendf(ctx->sb, " {\n");
            ctx->indent_level++;
            for (int i = 0; i < n->case_count; i++) {
                print_indent(ctx);
                Iron_MatchCase *mc = (Iron_MatchCase *)n->cases[i];
                print_node(ctx, mc->pattern);
                iron_strbuf_appendf(ctx->sb, " -> ");
                if (mc->body->kind == IRON_NODE_BLOCK) {
                    print_block(ctx, mc->body);
                } else {
                    print_node(ctx, mc->body);
                }
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            if (n->else_body) {
                print_indent(ctx);
                iron_strbuf_appendf(ctx->sb, "else -> ");
                if (n->else_body->kind == IRON_NODE_BLOCK) {
                    print_block(ctx, n->else_body);
                } else {
                    print_node(ctx, n->else_body);
                }
                iron_strbuf_appendf(ctx->sb, "\n");
            }
            ctx->indent_level--;
            print_indent(ctx);
            iron_strbuf_appendf(ctx->sb, "}");
            break;
        }

        case IRON_NODE_DEFER: {
            Iron_DeferStmt *n = (Iron_DeferStmt *)node;
            iron_strbuf_appendf(ctx->sb, "defer ");
            print_node(ctx, n->expr);
            break;
        }

        case IRON_NODE_FREE: {
            Iron_FreeStmt *n = (Iron_FreeStmt *)node;
            iron_strbuf_appendf(ctx->sb, "free ");
            print_node(ctx, n->expr);
            break;
        }

        case IRON_NODE_LEAK: {
            Iron_LeakStmt *n = (Iron_LeakStmt *)node;
            iron_strbuf_appendf(ctx->sb, "leak ");
            print_node(ctx, n->expr);
            break;
        }

        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *n = (Iron_SpawnStmt *)node;
            iron_strbuf_appendf(ctx->sb, "spawn(\"%s\"", n->name ? n->name : "");
            if (n->pool_expr) {
                iron_strbuf_appendf(ctx->sb, ", ");
                print_node(ctx, n->pool_expr);
            }
            iron_strbuf_appendf(ctx->sb, ") ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_BLOCK: {
            print_block(ctx, node);
            break;
        }

        /* ── Expressions ── */
        case IRON_NODE_INT_LIT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_IntLit *)node)->value);
            break;
        }

        case IRON_NODE_FLOAT_LIT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_FloatLit *)node)->value);
            break;
        }

        case IRON_NODE_STRING_LIT: {
            iron_strbuf_appendf(ctx->sb, "\"%s\"", ((Iron_StringLit *)node)->value);
            break;
        }

        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *n = (Iron_InterpString *)node;
            iron_strbuf_appendf(ctx->sb, "\"");
            for (int i = 0; i < n->part_count; i++) {
                Iron_Node *part = n->parts[i];
                if (part->kind == IRON_NODE_STRING_LIT) {
                    /* Literal segment: print without quotes */
                    iron_strbuf_appendf(ctx->sb, "%s",
                                        ((Iron_StringLit *)part)->value);
                } else {
                    /* Expression segment: wrap in {} */
                    iron_strbuf_appendf(ctx->sb, "{");
                    print_node(ctx, part);
                    iron_strbuf_appendf(ctx->sb, "}");
                }
            }
            iron_strbuf_appendf(ctx->sb, "\"");
            break;
        }

        case IRON_NODE_BOOL_LIT: {
            iron_strbuf_appendf(ctx->sb, "%s",
                                ((Iron_BoolLit *)node)->value ? "true" : "false");
            break;
        }

        case IRON_NODE_NULL_LIT: {
            iron_strbuf_appendf(ctx->sb, "null");
            break;
        }

        case IRON_NODE_IDENT: {
            iron_strbuf_appendf(ctx->sb, "%s", ((Iron_Ident *)node)->name);
            break;
        }

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *n = (Iron_BinaryExpr *)node;
            iron_strbuf_appendf(ctx->sb, "(");
            print_node(ctx, n->left);
            iron_strbuf_appendf(ctx->sb, " %s ", op_str(n->op));
            print_node(ctx, n->right);
            iron_strbuf_appendf(ctx->sb, ")");
            break;
        }

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *n = (Iron_UnaryExpr *)node;
            if ((Iron_TokenKind)n->op == IRON_TOK_NOT) {
                iron_strbuf_appendf(ctx->sb, "not ");
            } else {
                iron_strbuf_appendf(ctx->sb, "%s", op_str(n->op));
            }
            print_node(ctx, n->operand);
            break;
        }

        case IRON_NODE_CALL: {
            Iron_CallExpr *n = (Iron_CallExpr *)node;
            print_node(ctx, n->callee);
            print_args(ctx, n->args, n->arg_count);
            break;
        }

        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *n = (Iron_MethodCallExpr *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, ".%s", n->method);
            print_args(ctx, n->args, n->arg_count);
            break;
        }

        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *n = (Iron_FieldAccess *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, ".%s", n->field);
            break;
        }

        case IRON_NODE_INDEX: {
            Iron_IndexExpr *n = (Iron_IndexExpr *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, "[");
            print_node(ctx, n->index);
            iron_strbuf_appendf(ctx->sb, "]");
            break;
        }

        case IRON_NODE_SLICE: {
            Iron_SliceExpr *n = (Iron_SliceExpr *)node;
            print_node(ctx, n->object);
            iron_strbuf_appendf(ctx->sb, "[");
            if (n->start) print_node(ctx, n->start);
            iron_strbuf_appendf(ctx->sb, "..");
            if (n->end) print_node(ctx, n->end);
            iron_strbuf_appendf(ctx->sb, "]");
            break;
        }

        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *n = (Iron_LambdaExpr *)node;
            iron_strbuf_appendf(ctx->sb, "func");
            print_params(ctx, n->params, n->param_count);
            if (n->return_type) {
                iron_strbuf_appendf(ctx->sb, " -> ");
                print_type_ann(ctx, n->return_type);
            }
            iron_strbuf_appendf(ctx->sb, " ");
            print_block(ctx, n->body);
            break;
        }

        case IRON_NODE_HEAP: {
            Iron_HeapExpr *n = (Iron_HeapExpr *)node;
            iron_strbuf_appendf(ctx->sb, "heap ");
            print_node(ctx, n->inner);
            break;
        }

        case IRON_NODE_RC: {
            Iron_RcExpr *n = (Iron_RcExpr *)node;
            iron_strbuf_appendf(ctx->sb, "rc ");
            print_node(ctx, n->inner);
            break;
        }

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *n = (Iron_ComptimeExpr *)node;
            iron_strbuf_appendf(ctx->sb, "comptime ");
            print_node(ctx, n->inner);
            break;
        }

        case IRON_NODE_IS: {
            Iron_IsExpr *n = (Iron_IsExpr *)node;
            print_node(ctx, n->expr);
            iron_strbuf_appendf(ctx->sb, " is %s", n->type_name);
            break;
        }

        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *n = (Iron_AwaitExpr *)node;
            iron_strbuf_appendf(ctx->sb, "await ");
            print_node(ctx, n->handle);
            break;
        }

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *n = (Iron_ConstructExpr *)node;
            iron_strbuf_appendf(ctx->sb, "%s", n->type_name);
            if (n->generic_arg_count > 0) {
                iron_strbuf_appendf(ctx->sb, "[");
                for (int i = 0; i < n->generic_arg_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_type_ann(ctx, n->generic_args[i]);
                }
                iron_strbuf_appendf(ctx->sb, "]");
            }
            print_args(ctx, n->args, n->arg_count);
            break;
        }

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *n = (Iron_ArrayLit *)node;
            iron_strbuf_appendf(ctx->sb, "[");
            if (n->type_ann && n->size) {
                print_type_ann(ctx, n->type_ann);
                iron_strbuf_appendf(ctx->sb, "; ");
                print_node(ctx, n->size);
            } else {
                for (int i = 0; i < n->element_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_node(ctx, n->elements[i]);
                }
            }
            iron_strbuf_appendf(ctx->sb, "]");
            break;
        }

        case IRON_NODE_ERROR: {
            iron_strbuf_appendf(ctx->sb, "/* error */");
            break;
        }

        /* ── Helpers (printed inline above, but handle here as fallback) ── */
        case IRON_NODE_PARAM: {
            print_param(ctx, node);
            break;
        }

        case IRON_NODE_FIELD: {
            Iron_Field *f = (Iron_Field *)node;
            /* Phase 9 Plan 09-02 D-10: pub prefix on field. Iron_Field is
             * the only ast node that carries is_pub on the v3 surface
             * (parser.c:3529); FuncDecl / MethodDecl have no equivalent
             * because methods default public. */
            if (f->is_pub) iron_strbuf_appendf(ctx->sb, "pub ");
            iron_strbuf_appendf(ctx->sb, "%s %s", f->is_var ? "var" : "val", f->name);
            if (f->type_ann) {
                iron_strbuf_appendf(ctx->sb, ": ");
                print_type_ann(ctx, f->type_ann);
            }
            break;
        }

        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            print_node(ctx, mc->pattern);
            iron_strbuf_appendf(ctx->sb, " -> ");
            if (mc->body->kind == IRON_NODE_BLOCK) {
                print_block(ctx, mc->body);
            } else {
                print_node(ctx, mc->body);
            }
            break;
        }

        case IRON_NODE_ENUM_VARIANT: {
            Iron_EnumVariant *ev = (Iron_EnumVariant *)node;
            iron_strbuf_appendf(ctx->sb, "%s", ev->name);
            if (ev->payload_count > 0) {
                iron_strbuf_appendf(ctx->sb, "(");
                for (int j = 0; j < ev->payload_count; j++) {
                    if (j > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    print_node(ctx, ev->payload_type_anns[j]);
                }
                iron_strbuf_appendf(ctx->sb, ")");
            }
            break;
        }

        case IRON_NODE_TYPE_ANNOTATION: {
            print_type_ann(ctx, node);
            break;
        }

        case IRON_NODE_PATTERN: {
            Iron_Pattern *n = (Iron_Pattern *)node;
            if (n->enum_name) iron_strbuf_appendf(ctx->sb, "%s.", n->enum_name);
            iron_strbuf_appendf(ctx->sb, "%s", n->variant_name ? n->variant_name : "_");
            if (n->binding_count > 0) {
                iron_strbuf_appendf(ctx->sb, "(");
                for (int i = 0; i < n->binding_count; i++) {
                    if (i > 0) iron_strbuf_appendf(ctx->sb, ", ");
                    if (n->nested_patterns && n->nested_patterns[i]) {
                        print_node(ctx, n->nested_patterns[i]);
                    } else {
                        iron_strbuf_appendf(ctx->sb, "%s",
                            (n->binding_names && n->binding_names[i]) ? n->binding_names[i] : "_");
                    }
                }
                iron_strbuf_appendf(ctx->sb, ")");
            }
            break;
        }

        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *n = (Iron_EnumConstruct *)node;
            if (n->enum_name) iron_strbuf_appendf(ctx->sb, "%s.", n->enum_name);
            iron_strbuf_appendf(ctx->sb, "%s", n->variant_name ? n->variant_name : "");
            if (n->arg_count > 0) {
                print_args(ctx, n->args, n->arg_count);
            }
            break;
        }

        case IRON_NODE_COUNT:
            break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Phase 9 Plan 09-02 D-10 Approach 1: build the method-by-type-name index
 * if the print root is a top-level Iron_Program, NULL otherwise. Recursive
 * print of an AST subtree (e.g., a single FuncDecl) bypasses the merge —
 * methods would not be re-emitted inside an absent owning object body, so
 * they print at top level via the standalone METHOD_DECL arm. */
static MethodIndexEntry *build_print_index_for_root(Iron_Node *root) {
    if (!root) return NULL;
    if (root->kind != IRON_NODE_PROGRAM) return NULL;
    return method_index_build((Iron_Program *)root);
}

char *iron_print_ast(Iron_Node *root, const IronFmtOptions *opts, Iron_Arena *arena) {
    Iron_StrBuf       sb  = iron_strbuf_create(512);
    MethodIndexEntry *idx = build_print_index_for_root(root);
    PrintCtx          ctx = { &sb, 0, opts, idx };
    print_node(&ctx, root);
    method_index_free(idx);

    /* Copy result into arena */
    const char *result = iron_strbuf_get(&sb);
    size_t      len    = sb.len;
    char       *out    = (char *)iron_arena_alloc(arena, len + 1, 1);
    if (!out) iron_oom_abort("printer.c:iron_print_ast");
    memcpy(out, result, len + 1);
    iron_strbuf_free(&sb);
    return out;
}

void iron_print_ast_to_file(Iron_Node *root, const IronFmtOptions *opts, FILE *out) {
    Iron_StrBuf       sb  = iron_strbuf_create(512);
    MethodIndexEntry *idx = build_print_index_for_root(root);
    PrintCtx          ctx = { &sb, 0, opts, idx };
    print_node(&ctx, root);
    method_index_free(idx);
    fprintf(out, "%s", iron_strbuf_get(&sb));
    iron_strbuf_free(&sb);
}

/* ── Phase 94 LIB-02: pub-stub generator ─────────────────────────────────── */

/* Pull a decl's name for sorting, regardless of which top-level decl kind. */
static const char *stub_decl_name(Iron_Node *n) {
    if (!n) return "";
    /* Cast to int so -Werror=switch-enum accepts the default arm. */
    switch ((int)n->kind) {
        case IRON_NODE_FUNC_DECL:   return ((Iron_FuncDecl *)n)->name   ? ((Iron_FuncDecl *)n)->name   : "";
        case IRON_NODE_OBJECT_DECL: return ((Iron_ObjectDecl *)n)->name ? ((Iron_ObjectDecl *)n)->name : "";
        case IRON_NODE_ENUM_DECL:   return ((Iron_EnumDecl *)n)->name   ? ((Iron_EnumDecl *)n)->name   : "";
        default:                    return "";
    }
}

static int stub_cmp_decl_name(const void *a, const void *b) {
    Iron_Node *x = *(Iron_Node *const *)a;
    Iron_Node *y = *(Iron_Node *const *)b;
    return strcmp(stub_decl_name(x), stub_decl_name(y));
}

static int stub_cmp_method_name(const void *a, const void *b) {
    Iron_MethodDecl *x = *(Iron_MethodDecl *const *)a;
    Iron_MethodDecl *y = *(Iron_MethodDecl *const *)b;
    const char *xn = x->method_name ? x->method_name : "";
    const char *yn = y->method_name ? y->method_name : "";
    return strcmp(xn, yn);
}

/* Emit a parameter list: (name: Type, ...). Mirrors print_param/print_params
 * but writes directly to FILE *out so the stub generator avoids round-tripping
 * through the strbuf-backed PrintCtx machinery. */
static void stub_emit_param(FILE *out, Iron_Node *node) {
    if (!node || node->kind != IRON_NODE_PARAM) return;
    Iron_Param *p = (Iron_Param *)node;
    if (p->is_var) fprintf(out, "var ");
    fprintf(out, "%s", p->name ? p->name : "_");
    if (p->type_ann && p->type_ann->kind == IRON_NODE_TYPE_ANNOTATION) {
        Iron_TypeAnnotation *t = (Iron_TypeAnnotation *)p->type_ann;
        fprintf(out, ": ");
        if (t->is_array) {
            fprintf(out, "[%s", t->name ? t->name : "");
            fprintf(out, "]");
        } else {
            fprintf(out, "%s", t->name ? t->name : "");
            if (t->is_nullable) fprintf(out, "?");
            if (t->generic_arg_count > 0) {
                fprintf(out, "[");
                for (int i = 0; i < t->generic_arg_count; i++) {
                    if (i > 0) fprintf(out, ", ");
                    Iron_TypeAnnotation *g = (Iron_TypeAnnotation *)t->generic_args[i];
                    if (g) fprintf(out, "%s", g->name ? g->name : "");
                }
                fprintf(out, "]");
            }
        }
    }
}

static void stub_emit_params(FILE *out, Iron_Node **params, int count) {
    fprintf(out, "(");
    for (int i = 0; i < count; i++) {
        if (i > 0) fprintf(out, ", ");
        stub_emit_param(out, params[i]);
    }
    fprintf(out, ")");
}

static void stub_emit_type_ann(FILE *out, Iron_Node *node) {
    if (!node || node->kind != IRON_NODE_TYPE_ANNOTATION) return;
    Iron_TypeAnnotation *t = (Iron_TypeAnnotation *)node;
    if (t->is_array) {
        fprintf(out, "[%s]", t->name ? t->name : "");
    } else {
        fprintf(out, "%s", t->name ? t->name : "");
        if (t->is_nullable) fprintf(out, "?");
        if (t->generic_arg_count > 0) {
            fprintf(out, "[");
            for (int i = 0; i < t->generic_arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                stub_emit_type_ann(out, t->generic_args[i]);
            }
            fprintf(out, "]");
        }
    }
}

static void stub_emit_generic_params(FILE *out, Iron_Node **gps, int count) {
    if (count == 0) return;
    fprintf(out, "[");
    for (int i = 0; i < count; i++) {
        if (i > 0) fprintf(out, ", ");
        if (gps[i]->kind == IRON_NODE_IDENT) {
            fprintf(out, "%s", ((Iron_Ident *)gps[i])->name);
        } else if (gps[i]->kind == IRON_NODE_TYPE_ANNOTATION) {
            stub_emit_type_ann(out, gps[i]);
        }
    }
    fprintf(out, "]");
}

/* Emit a top-level pub func: signature + empty body. */
static void stub_emit_func(FILE *out, Iron_FuncDecl *fd) {
    fprintf(out, "pub func %s", fd->name ? fd->name : "?");
    stub_emit_generic_params(out, fd->generic_params, fd->generic_param_count);
    stub_emit_params(out, fd->params, fd->param_count);
    if (fd->return_type) {
        fprintf(out, " -> ");
        stub_emit_type_ann(out, fd->return_type);
    }
    fprintf(out, " {\n}\n\n");
}

/* Emit a method nested inside an object stub. Indented four spaces.
 * Init methods print as `pub init(...) {}` (or `pub init.<name>(...)` for
 * named inits); plain methods print as `pub func <name>(...) {}`.
 *
 * Round-trip critical: in-block methods carry a parser-synthesized `self`
 * receiver as params[0] (parser.c:3098-3134). When the consumer re-parses
 * the stub, the parser will synthesize ANOTHER `self`, producing duplicate
 * receivers. We must therefore skip params[0] when emitting nested method
 * decls so the round-trip is identity-preserving. */
static void stub_emit_method(FILE *out, Iron_MethodDecl *md) {
    fprintf(out, "    pub ");
    if (md->is_init) {
        if (md->init_name) {
            fprintf(out, "init.%s", md->init_name);
        } else {
            fprintf(out, "init");
        }
    } else {
        fprintf(out, "func %s", md->method_name ? md->method_name : "?");
    }
    stub_emit_generic_params(out, md->generic_params, md->generic_param_count);

    /* Skip params[0] when it's the synthesized `self` receiver. Detect via
     * is_receiver_form (Phase 82 in-block + Phase 79 receiver-form both set
     * this) AND a "self"-named first param. The defensive name check covers
     * the `func (recv: T) name()` form where the receiver might be named
     * something other than "self" (in which case we keep all params). */
    Iron_Node **emit_params = md->params;
    int emit_count = md->param_count;
    if (md->is_receiver_form && md->param_count > 0) {
        Iron_Param *p0 = (Iron_Param *)md->params[0];
        if (p0 && p0->name && strcmp(p0->name, "self") == 0) {
            emit_params = md->params + 1;
            emit_count  = md->param_count - 1;
        }
    }
    stub_emit_params(out, emit_params, emit_count);

    if (md->return_type) {
        fprintf(out, " -> ");
        stub_emit_type_ann(out, md->return_type);
    }
    fprintf(out, " {\n    }\n");
}

/* Emit a pub object: header line, fields preserved (type only), methods
 * regrouped from prog->decls by type_name (alphabetic by method_name). */
static void stub_emit_object(FILE *out, Iron_ObjectDecl *od, Iron_Program *prog) {
    fprintf(out, "pub object %s", od->name ? od->name : "?");
    stub_emit_generic_params(out, od->generic_params, od->generic_param_count);
    fprintf(out, " {\n");

    /* Fields: preserve types (no values; field initializers are not part of
     * the public surface — consumers construct via pub init). */
    for (int fi = 0; fi < od->field_count; fi++) {
        Iron_Field *f = (Iron_Field *)od->fields[fi];
        if (!f) continue;
        fprintf(out, "    %s %s",
                f->is_var ? "var" : "val",
                f->name ? f->name : "_");
        if (f->type_ann) {
            fprintf(out, ": ");
            stub_emit_type_ann(out, f->type_ann);
        }
        fprintf(out, "\n");
    }

    /* Methods: walk prog->decls, collect any IRON_NODE_METHOD_DECL whose
     * type_name == od->name && is_pub && !is_array_extension && !is_patch_member.
     * Pitfall 4 lock: is_patch_member excludes methods sourced from
     * `pub patch object T { ... }` even when a non-patch `pub object T`
     * coexists in the same source. Sort alphabetically by method_name for
     * diff stability. */
    Iron_MethodDecl **methods = NULL;
    size_t mc = 0, mcap = 0;
    for (int k = 0; k < prog->decl_count; k++) {
        Iron_Node *m = prog->decls[k];
        if (!m || m->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)m;
        if (!md->is_pub) continue;
        if (md->is_array_extension) continue;
        if (md->is_patch_member) continue;  /* Pitfall 4: drop patch-sourced methods */
        if (!md->type_name || strcmp(md->type_name, od->name) != 0) continue;
        if (mc == mcap) {
            mcap = mcap ? mcap * 2 : 4;
            methods = (Iron_MethodDecl **)realloc(methods, mcap * sizeof(*methods));
        }
        methods[mc++] = md;
    }
    if (mc > 1) qsort(methods, mc, sizeof(*methods), stub_cmp_method_name);
    for (size_t mi = 0; mi < mc; mi++) {
        stub_emit_method(out, methods[mi]);
    }
    free(methods);

    fprintf(out, "}\n\n");
}

/* Emit a pub enum: full variant list (variants are pure type defs; nothing
 * to strip). Uses payload type-annotation names directly. */
static void stub_emit_enum(FILE *out, Iron_EnumDecl *ed) {
    fprintf(out, "pub enum %s", ed->name ? ed->name : "?");
    stub_emit_generic_params(out, ed->generic_params, ed->generic_param_count);
    fprintf(out, " {\n");
    for (int i = 0; i < ed->variant_count; i++) {
        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
        if (!ev) continue;
        fprintf(out, "    %s", ev->name ? ev->name : "?");
        if (ev->payload_count > 0) {
            fprintf(out, "(");
            for (int j = 0; j < ev->payload_count; j++) {
                if (j > 0) fprintf(out, ", ");
                stub_emit_type_ann(out, ev->payload_type_anns[j]);
            }
            fprintf(out, ")");
        }
        fprintf(out, ",\n");
    }
    fprintf(out, "}\n\n");
}

int iron_print_pub_stubs(Iron_Program *prog, FILE *out,
                         const char *pkg_name, const char *pkg_version) {
    fprintf(out, "-- iron-stub: auto-generated from src/lib.iron of %s v%s. Do not edit.\n\n",
            pkg_name ? pkg_name : "?", pkg_version ? pkg_version : "?");

    if (!prog) return 0;

    /* 1. Bucket pub top-level decls by kind. is_patch objects are excluded
     *    (Pitfall 4 lock); their grouped methods are implicitly excluded
     *    because the per-object method walk only fires for collected pub
     *    objects. */
    Iron_Node **pub_funcs   = NULL; size_t fc = 0, fcap = 0;
    Iron_Node **pub_objects = NULL; size_t oc = 0, ocap = 0;
    Iron_Node **pub_enums   = NULL; size_t ec = 0, ecap = 0;

    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d) continue;
        /* Cast to int so -Werror=switch-enum accepts the default arm. */
        switch ((int)d->kind) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
                if (fd->is_pub && !fd->is_extern) {
                    if (fc == fcap) {
                        fcap = fcap ? fcap * 2 : 8;
                        pub_funcs = (Iron_Node **)realloc(pub_funcs, fcap * sizeof(*pub_funcs));
                    }
                    pub_funcs[fc++] = d;
                }
                break;
            }
            case IRON_NODE_OBJECT_DECL: {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
                /* Pitfall 4: skip ANY object with is_patch == true even if
                 * is_pub — patches are implementation details, not public API. */
                if (od->is_pub && !od->is_patch) {
                    if (oc == ocap) {
                        ocap = ocap ? ocap * 2 : 8;
                        pub_objects = (Iron_Node **)realloc(pub_objects, ocap * sizeof(*pub_objects));
                    }
                    pub_objects[oc++] = d;
                }
                break;
            }
            case IRON_NODE_ENUM_DECL: {
                Iron_EnumDecl *ed = (Iron_EnumDecl *)d;
                if (ed->is_pub) {
                    if (ec == ecap) {
                        ecap = ecap ? ecap * 2 : 8;
                        pub_enums = (Iron_Node **)realloc(pub_enums, ecap * sizeof(*pub_enums));
                    }
                    pub_enums[ec++] = d;
                }
                break;
            }
            default: break;
        }
    }

    /* 2. Sort each bucket alphabetically by name (case-sensitive strcmp / C locale). */
    if (fc > 1) qsort(pub_funcs,   fc, sizeof(*pub_funcs),   stub_cmp_decl_name);
    if (oc > 1) qsort(pub_objects, oc, sizeof(*pub_objects), stub_cmp_decl_name);
    if (ec > 1) qsort(pub_enums,   ec, sizeof(*pub_enums),   stub_cmp_decl_name);

    int emitted = 0;

    /* 3. Emit enums first (pure type defs, no body to strip). */
    for (size_t i = 0; i < ec; i++) {
        stub_emit_enum(out, (Iron_EnumDecl *)pub_enums[i]);
        emitted++;
    }

    /* 4. Emit objects with regrouped methods inside braces. */
    for (size_t i = 0; i < oc; i++) {
        stub_emit_object(out, (Iron_ObjectDecl *)pub_objects[i], prog);
        emitted++;
    }

    /* 5. Emit free funcs (no enclosing object). */
    for (size_t i = 0; i < fc; i++) {
        stub_emit_func(out, (Iron_FuncDecl *)pub_funcs[i]);
        emitted++;
    }

    free(pub_funcs);
    free(pub_objects);
    free(pub_enums);
    return emitted;
}
