#include "analyzer/expand.h"

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/printer.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    Iron_Node  *type;
} TypeBinding;

typedef struct {
    Iron_Program    *program;
    Iron_Arena      *arena;
    Iron_DiagList   *diags;
    const char      *current_type;
    const char      *field_var;
    Iron_Field      *field;
    int              field_index;
    TypeBinding     *bindings;
    int              binding_count;
    int              changed;
} ExpandCtx;

const char *iron_reflect_field_doc(const Iron_Field *field) {
    (void)field;
    return NULL;
}

const char *iron_reflect_field_tag(const Iron_Field *field, const char *key) {
    (void)field;
    (void)key;
    return NULL;
}

const char *iron_reflect_object_doc(const Iron_ObjectDecl *object) {
    (void)object;
    return NULL;
}

const char *iron_reflect_object_tag(const Iron_ObjectDecl *object,
                                    const char *key) {
    (void)object;
    (void)key;
    return NULL;
}

static Iron_ObjectDecl *find_object(Iron_Program *program, const char *name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (!od->is_patch && od->name && strcmp(od->name, name) == 0) return od;
    }
    return NULL;
}

static Iron_InterfaceDecl *find_interface(Iron_Program *program,
                                           const char *name) {
    if (!program || !name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_INTERFACE_DECL) continue;
        Iron_InterfaceDecl *id = (Iron_InterfaceDecl *)d;
        if (id->name && strcmp(id->name, name) == 0) return id;
    }
    return NULL;
}

static bool object_implements(Iron_ObjectDecl *od, const char *iface) {
    if (!od || !iface) return false;
    for (int i = 0; i < od->implements_count; i++)
        if (od->implements_names[i] &&
            strcmp(od->implements_names[i], iface) == 0) return true;
    return false;
}

static Iron_Node *make_bool(ExpandCtx *ctx, Iron_Span span, bool value) {
    Iron_BoolLit *n = ARENA_ALLOC(ctx->arena, Iron_BoolLit);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind = IRON_NODE_BOOL_LIT;
    n->span = span;
    n->value = value;
    return (Iron_Node *)n;
}

static Iron_Node *make_null(ExpandCtx *ctx, Iron_Span span) {
    Iron_NullLit *n = ARENA_ALLOC(ctx->arena, Iron_NullLit);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind = IRON_NODE_NULL_LIT;
    n->span = span;
    return (Iron_Node *)n;
}

static Iron_Node *make_int(ExpandCtx *ctx, Iron_Span span, int value) {
    Iron_IntLit *n = ARENA_ALLOC(ctx->arena, Iron_IntLit);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    n->kind = IRON_NODE_INT_LIT;
    n->span = span;
    n->value = iron_arena_strdup(ctx->arena, buf, strlen(buf));
    return (Iron_Node *)n;
}

static Iron_Node *make_string(ExpandCtx *ctx, Iron_Span span,
                              const char *value) {
    Iron_StringLit *n = ARENA_ALLOC(ctx->arena, Iron_StringLit);
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->kind = IRON_NODE_STRING_LIT;
    n->span = span;
    n->value = iron_arena_strdup(ctx->arena, value ? value : "",
                                 strlen(value ? value : ""));
    return (Iron_Node *)n;
}

static bool ann_nullable(Iron_Node *n) {
    return n && n->kind == IRON_NODE_TYPE_ANNOTATION &&
           ((Iron_TypeAnnotation *)n)->is_nullable;
}

static Iron_Node *binding_type(ExpandCtx *ctx, const char *name) {
    for (int i = ctx->binding_count - 1; i >= 0; i--)
        if (ctx->bindings[i].name && strcmp(ctx->bindings[i].name, name) == 0)
            return ctx->bindings[i].type;
    return NULL;
}

static Iron_Node *subst_type(ExpandCtx *ctx, Iron_Node *node) {
    if (!node || node->kind != IRON_NODE_TYPE_ANNOTATION) return node;
    Iron_TypeAnnotation *a = (Iron_TypeAnnotation *)node;
    if (a->reflected_field) {
        if (ctx->field && ctx->field_var &&
            strcmp(a->reflected_field, ctx->field_var) == 0)
            return ctx->field->type_ann;
        return node;
    }
    Iron_Node *bound = a->name ? binding_type(ctx, a->name) : NULL;
    if (bound && !a->is_array && !a->is_pointer && !a->is_rc &&
        !a->is_weak_rc && !a->is_func && !a->is_tuple &&
        a->generic_arg_count == 0 && !a->is_nullable)
        return bound;
    for (int i = 0; i < a->generic_arg_count; i++)
        a->generic_args[i] = subst_type(ctx, a->generic_args[i]);
    for (int i = 0; i < a->func_param_count; i++)
        a->func_params[i] = subst_type(ctx, a->func_params[i]);
    for (int i = 0; i < a->tuple_elem_count; i++)
        a->tuple_elems[i] = subst_type(ctx, a->tuple_elems[i]);
    a->func_return = subst_type(ctx, a->func_return);
    a->pointer_pointee = subst_type(ctx, a->pointer_pointee);
    a->weak_rc_inner = subst_type(ctx, a->weak_rc_inner);
    a->rc_inner = subst_type(ctx, a->rc_inner);
    a->array_elem_ann = subst_type(ctx, a->array_elem_ann);
    return node;
}

static bool ident_named(Iron_Node *n, const char *name) {
    return n && n->kind == IRON_NODE_IDENT && ((Iron_Ident *)n)->name &&
           strcmp(((Iron_Ident *)n)->name, name) == 0;
}

static Iron_ObjectDecl *fields_target(ExpandCtx *ctx, Iron_Node *iterable,
                                      bool report) {
    if (!iterable || iterable->kind != IRON_NODE_CALL) goto bad;
    Iron_CallExpr *call = (Iron_CallExpr *)iterable;
    if (!ident_named(call->callee, "fields") || call->arg_count != 1) goto bad;
    Iron_Node *arg = call->args[0];
    const char *name = NULL;
    if (arg && arg->kind == IRON_NODE_IDENT) name = ((Iron_Ident *)arg)->name;
    if (name && strcmp(name, "Self") == 0) name = ctx->current_type;
    Iron_ObjectDecl *od = find_object(ctx->program, name);
    if (od) return od;
    /* A generic template is intentionally deferred until its clone has a
     * concrete type spelling. */
    if (name && !ctx->current_type) return NULL;
bad:
    if (report) {
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_REFLECTION_TYPE,
                       iterable ? iterable->span : (Iron_Span){0},
                       "fields(T) requires a concrete object type",
                       "use fields(Self), a concrete object type, or a type parameter after monomorphisation");
    }
    return NULL;
}

typedef struct { Iron_Span span; } SpanCtx;
static bool set_span_visit(Iron_Visitor *v, Iron_Node *n) {
    n->span = ((SpanCtx *)v->ctx)->span;
    return true;
}

static Iron_Node *clone_block(ExpandCtx *ctx, Iron_Node *body,
                              Iron_Span span) {
    char *printed = iron_print_ast(body, NULL, ctx->arena);
    if (!printed) return NULL;
    Iron_StrBuf sb = iron_strbuf_create(strlen(printed) + 64);
    iron_strbuf_appendf(&sb, "func __reflect_clone() %s\n", printed);
    char *src = iron_arena_strdup(ctx->arena, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!src) return NULL;
    Iron_Lexer lexer = iron_lexer_create(src, span.filename ? span.filename :
                                         "<reflection>", ctx->arena, ctx->diags);
    Iron_Token *tokens = iron_lex_all(&lexer);
    Iron_Parser parser = iron_parser_create(tokens, (int)arrlen(tokens), src,
                                            span.filename ? span.filename :
                                            "<reflection>", ctx->arena,
                                            ctx->diags);
    parser.generated = true;
    Iron_Node *root = iron_parse(&parser);
    arrfree(tokens);
    if (!root || root->kind != IRON_NODE_PROGRAM) return NULL;
    Iron_Program *p = (Iron_Program *)root;
    Iron_Node *out = NULL;
    for (int i = 0; i < p->decl_count; i++) {
        if (p->decls[i] && p->decls[i]->kind == IRON_NODE_FUNC_DECL) {
            out = ((Iron_FuncDecl *)p->decls[i])->body;
            break;
        }
    }
    if (out) {
        SpanCtx sc = { span };
        Iron_Visitor v = { .ctx = &sc, .visit_node = set_span_visit,
                           .post_visit = NULL };
        iron_ast_walk(out, &v);
    }
    return out;
}

static Iron_Node *expand_expr(ExpandCtx *ctx, Iron_Node *node);
static void expand_block(ExpandCtx *ctx, Iron_Node *node);

static Iron_Node *metadata_value(ExpandCtx *ctx, Iron_FieldAccess *fa) {
    if (!ctx->field || !ctx->field_var || !ident_named(fa->object, ctx->field_var) ||
        !fa->field) return NULL;
    if (strcmp(fa->field, "name") == 0)
        return make_string(ctx, fa->span, ctx->field->name);
    if (strcmp(fa->field, "index") == 0)
        return make_int(ctx, fa->span, ctx->field_index);
    if (strcmp(fa->field, "is_var") == 0)
        return make_bool(ctx, fa->span, ctx->field->is_var);
    if (strcmp(fa->field, "is_nullable") == 0)
        return make_bool(ctx, fa->span, ann_nullable(ctx->field->type_ann));
    if (strcmp(fa->field, "is_pub") == 0)
        return make_bool(ctx, fa->span, ctx->field->is_pub);
    if (strcmp(fa->field, "visibility") == 0)
        return make_string(ctx, fa->span,
                           ctx->field->is_pub ? "public" : "private");
    if (strcmp(fa->field, "doc") == 0) {
        const char *doc = iron_reflect_field_doc(ctx->field);
        return doc ? make_string(ctx, fa->span, doc) : make_null(ctx, fa->span);
    }
    return NULL;
}

static bool typeinfo_call(Iron_Node *node, const char **out_type) {
    if (!node || node->kind != IRON_NODE_CALL) return false;
    Iron_CallExpr *c = (Iron_CallExpr *)node;
    if (!ident_named(c->callee, "typeinfo") || c->arg_count != 1 ||
        !c->args[0] || c->args[0]->kind != IRON_NODE_IDENT) return false;
    *out_type = ((Iron_Ident *)c->args[0])->name;
    return true;
}

static Iron_Node *expand_expr(ExpandCtx *ctx, Iron_Node *node) {
    if (!node) return NULL;
    switch ((int)node->kind) {
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            fa->object = expand_expr(ctx, fa->object);
            Iron_Node *meta = metadata_value(ctx, fa);
            if (meta) return meta;
            if (fa->reflected_field && ctx->field && ctx->field_var &&
                strcmp(fa->reflected_field, ctx->field_var) == 0) {
                fa->field = ctx->field->name;
                fa->reflected_field = NULL;
                fa->is_generated = true;
                ctx->changed++;
            }
            const char *tn = NULL;
            if (fa->field && typeinfo_call(fa->object, &tn) &&
                strcmp(fa->field, "doc") == 0) {
                if (strcmp(tn, "Self") == 0) tn = ctx->current_type;
                Iron_ObjectDecl *od = find_object(ctx->program, tn);
                const char *doc = iron_reflect_object_doc(od);
                return doc ? make_string(ctx, fa->span, doc)
                           : make_null(ctx, fa->span);
            }
            return node;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            mc->object = expand_expr(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++)
                mc->args[i] = expand_expr(ctx, mc->args[i]);
            for (int i = 0; i < mc->generic_arg_count; i++)
                mc->generic_args[i] = subst_type(ctx, mc->generic_args[i]);
            if (ctx->field && ctx->field_var && ident_named(mc->object, ctx->field_var) &&
                mc->method && strcmp(mc->method, "tag") == 0) {
                const char *key = NULL;
                if (mc->arg_count == 1 && mc->args[0] &&
                    mc->args[0]->kind == IRON_NODE_STRING_LIT)
                    key = ((Iron_StringLit *)mc->args[0])->value;
                const char *tag = key ? iron_reflect_field_tag(ctx->field, key) : NULL;
                return tag ? make_string(ctx, mc->span, tag)
                           : make_null(ctx, mc->span);
            }
            const char *tn = NULL;
            if (mc->method && strcmp(mc->method, "tag") == 0 &&
                typeinfo_call(mc->object, &tn)) {
                if (strcmp(tn, "Self") == 0) tn = ctx->current_type;
                Iron_ObjectDecl *od = find_object(ctx->program, tn);
                const char *key = mc->arg_count == 1 && mc->args[0] &&
                                  mc->args[0]->kind == IRON_NODE_STRING_LIT
                                  ? ((Iron_StringLit *)mc->args[0])->value : NULL;
                const char *tag = key ? iron_reflect_object_tag(od, key) : NULL;
                return tag ? make_string(ctx, mc->span, tag)
                           : make_null(ctx, mc->span);
            }
            if (mc->method && strcmp(mc->method, "build") == 0 &&
                mc->arg_count == 0 && mc->object->kind == IRON_NODE_IDENT)
                return mc->object;
            return node;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *c = (Iron_CallExpr *)node;
            c->callee = expand_expr(ctx, c->callee);
            for (int i = 0; i < c->arg_count; i++)
                c->args[i] = expand_expr(ctx, c->args[i]);
            return node;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *b = (Iron_BinaryExpr *)node;
            b->left = expand_expr(ctx, b->left);
            b->right = expand_expr(ctx, b->right);
            return node;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *u = (Iron_UnaryExpr *)node;
            u->operand = expand_expr(ctx, u->operand);
            return node;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *i = (Iron_IndexExpr *)node;
            i->object = expand_expr(ctx, i->object);
            i->index = expand_expr(ctx, i->index);
            for (int k = 0; k < i->type_arg_count; k++)
                i->type_args[k] = subst_type(ctx, i->type_args[k]);
            return node;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *s = (Iron_SliceExpr *)node;
            s->object = expand_expr(ctx, s->object);
            s->start = expand_expr(ctx, s->start);
            s->end = expand_expr(ctx, s->end);
            return node;
        }
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *s = (Iron_InterpString *)node;
            for (int i = 0; i < s->part_count; i++)
                s->parts[i] = expand_expr(ctx, s->parts[i]);
            return node;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *a = (Iron_ArrayLit *)node;
            a->type_ann = subst_type(ctx, a->type_ann);
            a->size = expand_expr(ctx, a->size);
            for (int i = 0; i < a->element_count; i++)
                a->elements[i] = expand_expr(ctx, a->elements[i]);
            return node;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *c = (Iron_ConstructExpr *)node;
            for (int i = 0; i < c->arg_count; i++)
                c->args[i] = expand_expr(ctx, c->args[i]);
            for (int i = 0; i < c->generic_arg_count; i++)
                c->generic_args[i] = subst_type(ctx, c->generic_args[i]);
            return node;
        }
        case IRON_NODE_IS:
            ((Iron_IsExpr *)node)->expr = expand_expr(ctx, ((Iron_IsExpr *)node)->expr);
            return node;
        case IRON_NODE_COMPTIME:
            ((Iron_ComptimeExpr *)node)->inner =
                expand_expr(ctx, ((Iron_ComptimeExpr *)node)->inner);
            return node;
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *l = (Iron_LambdaExpr *)node;
            for (int i = 0; i < l->param_count; i++) {
                Iron_Param *p = (Iron_Param *)l->params[i];
                p->type_ann = subst_type(ctx, p->type_ann);
            }
            l->return_type = subst_type(ctx, l->return_type);
            expand_block(ctx, l->body);
            return node;
        }
        default:
            return node;
    }
}

static bool const_value(Iron_Node *n, bool *out) {
    if (!n) return false;
    if (n->kind == IRON_NODE_BOOL_LIT) {
        *out = ((Iron_BoolLit *)n)->value;
        return true;
    }
    if (n->kind == IRON_NODE_UNARY &&
        ((Iron_UnaryExpr *)n)->op == IRON_TOK_NOT) {
        bool v;
        if (!const_value(((Iron_UnaryExpr *)n)->operand, &v)) return false;
        *out = !v;
        return true;
    }
    if (n->kind != IRON_NODE_BINARY) return false;
    Iron_BinaryExpr *b = (Iron_BinaryExpr *)n;
    if (b->op == IRON_TOK_AND || b->op == IRON_TOK_OR) {
        bool l, r;
        if (!const_value(b->left, &l) || !const_value(b->right, &r)) return false;
        *out = b->op == IRON_TOK_AND ? l && r : l || r;
        return true;
    }
    if (b->op != IRON_TOK_EQUALS && b->op != IRON_TOK_NOT_EQUALS) return false;
    bool eq = false, known = true;
    if (b->left->kind == IRON_NODE_NULL_LIT || b->right->kind == IRON_NODE_NULL_LIT)
        eq = b->left->kind == b->right->kind;
    else if (b->left->kind == IRON_NODE_BOOL_LIT && b->right->kind == IRON_NODE_BOOL_LIT)
        eq = ((Iron_BoolLit *)b->left)->value == ((Iron_BoolLit *)b->right)->value;
    else if (b->left->kind == IRON_NODE_STRING_LIT && b->right->kind == IRON_NODE_STRING_LIT)
        eq = strcmp(((Iron_StringLit *)b->left)->value,
                    ((Iron_StringLit *)b->right)->value) == 0;
    else if (b->left->kind == IRON_NODE_INT_LIT && b->right->kind == IRON_NODE_INT_LIT)
        eq = strcmp(((Iron_IntLit *)b->left)->value,
                    ((Iron_IntLit *)b->right)->value) == 0;
    else known = false;
    if (!known) return false;
    *out = b->op == IRON_TOK_EQUALS ? eq : !eq;
    return true;
}

static bool is_binder(ExpandCtx *ctx, Iron_TypeAnnotation *p) {
    return p && p->name && strlen(p->name) == 1 &&
           isupper((unsigned char)p->name[0]) &&
           !find_object(ctx->program, p->name) &&
           !find_interface(ctx->program, p->name) &&
           strcmp(p->name, "I") != 0;
}

static Iron_Node *without_nullable(ExpandCtx *ctx, Iron_Node *node) {
    Iron_TypeAnnotation *src = (Iron_TypeAnnotation *)node;
    Iron_TypeAnnotation *copy = ARENA_ALLOC(ctx->arena, Iron_TypeAnnotation);
    if (!copy) return node;
    *copy = *src;
    copy->is_nullable = false;
    return (Iron_Node *)copy;
}

static bool match_type(ExpandCtx *ctx, Iron_Node *pattern, Iron_Node *actual,
                       TypeBinding **new_bindings) {
    if (!pattern || !actual || pattern->kind != IRON_NODE_TYPE_ANNOTATION ||
        actual->kind != IRON_NODE_TYPE_ANNOTATION) return false;
    Iron_TypeAnnotation *p = (Iron_TypeAnnotation *)pattern;
    Iron_TypeAnnotation *a = (Iron_TypeAnnotation *)actual;
    if (p->is_nullable) {
        if (!a->is_nullable) return false;
        Iron_TypeAnnotation pc = *p;
        pc.is_nullable = false;
        return match_type(ctx, (Iron_Node *)&pc, without_nullable(ctx, actual),
                          new_bindings);
    }
    if (a->is_nullable) return false;
    if (is_binder(ctx, p) && !p->is_array && p->generic_arg_count == 0) {
        TypeBinding b = { p->name, actual };
        arrput(*new_bindings, b);
        return true;
    }
    if (p->is_array != a->is_array) return false;
    if (p->is_array) {
        Iron_Node *pe = p->array_elem_ann;
        Iron_Node *ae = a->array_elem_ann;
        if (!pe && p->name) {
            Iron_TypeAnnotation *x = ARENA_ALLOC(ctx->arena, Iron_TypeAnnotation);
            memset(x, 0, sizeof(*x)); x->kind = IRON_NODE_TYPE_ANNOTATION; x->name = p->name;
            pe = (Iron_Node *)x;
        }
        if (!ae && a->name) {
            Iron_TypeAnnotation *x = ARENA_ALLOC(ctx->arena, Iron_TypeAnnotation);
            memset(x, 0, sizeof(*x)); x->kind = IRON_NODE_TYPE_ANNOTATION; x->name = a->name;
            ae = (Iron_Node *)x;
        }
        return match_type(ctx, pe, ae, new_bindings);
    }
    if (p->name && find_interface(ctx->program, p->name)) {
        Iron_ObjectDecl *od = find_object(ctx->program, a->name);
        return object_implements(od, p->name);
    }
    if (!p->name || !a->name || strcmp(p->name, a->name) != 0 ||
        p->generic_arg_count != a->generic_arg_count) return false;
    for (int i = 0; i < p->generic_arg_count; i++)
        if (!match_type(ctx, p->generic_args[i], a->generic_args[i], new_bindings))
            return false;
    return true;
}

static bool comptime_error(ExpandCtx *ctx, Iron_Node *n) {
    if (!n || n->kind != IRON_NODE_COMPTIME) return false;
    Iron_Node *inner = ((Iron_ComptimeExpr *)n)->inner;
    if (!inner || inner->kind != IRON_NODE_CALL) return false;
    Iron_CallExpr *c = (Iron_CallExpr *)inner;
    if (!ident_named(c->callee, "error")) return false;
    const char *msg = "compile-time error";
    if (c->arg_count == 1 && c->args[0] && c->args[0]->kind == IRON_NODE_STRING_LIT)
        msg = ((Iron_StringLit *)c->args[0])->value;
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                   IRON_ERR_COMPTIME_ERROR,
                   ctx->field ? ctx->field->span : n->span, msg,
                   "this error was selected during compile-time reflection expansion");
    ctx->changed++;
    return true;
}

static void append_block_statements(Iron_Node ***out, Iron_Node *body) {
    if (!body || body->kind != IRON_NODE_BLOCK) return;
    Iron_Block *b = (Iron_Block *)body;
    for (int i = 0; i < b->stmt_count; i++) arrput(*out, b->stmts[i]);
}

static void expand_stmt(ExpandCtx *ctx, Iron_Node *stmt, Iron_Node ***out) {
    if (!stmt) return;
    if (stmt->kind == IRON_NODE_FOR && ((Iron_ForStmt *)stmt)->is_comptime) {
        Iron_ForStmt *f = (Iron_ForStmt *)stmt;
        Iron_ObjectDecl *od = fields_target(ctx, f->iterable, ctx->current_type != NULL);
        if (!od) { arrput(*out, stmt); return; }
        for (int i = 0; i < od->field_count; i++) {
            Iron_Field *field = (Iron_Field *)od->fields[i];
            Iron_Node *copy = clone_block(ctx, f->body,
                                          field ? field->span : stmt->span);
            if (!copy) continue;
            ExpandCtx inner = *ctx;
            inner.field_var = f->var_name;
            inner.field = field;
            inner.field_index = i;
            expand_block(&inner, copy);
            ctx->changed += inner.changed + 1;
            append_block_statements(out, copy);
        }
        return;
    }
    if (stmt->kind == IRON_NODE_IF && ((Iron_IfStmt *)stmt)->is_comptime) {
        Iron_IfStmt *s = (Iron_IfStmt *)stmt;
        s->condition = expand_expr(ctx, s->condition);
        bool take = false;
        if (!const_value(s->condition, &take)) {
            iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                           IRON_ERR_COMPTIME_CONDITION, s->condition->span,
                           "comptime if condition is not a compile-time reflection constant",
                           "use field metadata, tag/doc null tests, boolean operators, or literal comparisons");
            return;
        }
        Iron_Node *chosen = take ? s->body : s->else_body;
        if (chosen) {
            Iron_Node *copy = clone_block(ctx, chosen,
                                          ctx->field ? ctx->field->span : s->span);
            expand_block(ctx, copy);
            append_block_statements(out, copy);
        }
        ctx->changed++;
        return;
    }
    if (stmt->kind == IRON_NODE_MATCH && ((Iron_MatchStmt *)stmt)->is_comptime) {
        Iron_MatchStmt *m = (Iron_MatchStmt *)stmt;
        Iron_Node *subject = subst_type(ctx, m->subject);
        Iron_Node *chosen = NULL;
        TypeBinding *picked = NULL;
        for (int i = 0; i < m->case_count && !chosen; i++) {
            Iron_MatchCase *mc = (Iron_MatchCase *)m->cases[i];
            TypeBinding *trial = NULL;
            if (match_type(ctx, mc->pattern, subject, &trial)) {
                chosen = mc->body;
                picked = trial;
            } else arrfree(trial);
        }
        if (!chosen) chosen = m->else_body;
        if (!chosen) {
            iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                           IRON_ERR_COMPTIME_MATCH,
                           ctx->field ? ctx->field->span : m->span,
                           "no comptime match arm applies to this type",
                           "add a matching type pattern or an else arm");
            arrfree(picked);
            return;
        }
        Iron_Node *copy = clone_block(ctx, chosen,
                                      ctx->field ? ctx->field->span : m->span);
        int old_count = ctx->binding_count;
        TypeBinding *old = ctx->bindings;
        if (picked) {
            int n = (int)arrlen(picked);
            TypeBinding *all = NULL;
            for (int i = 0; i < old_count; i++) arrput(all, old[i]);
            for (int i = 0; i < n; i++) arrput(all, picked[i]);
            ctx->bindings = all;
            ctx->binding_count = (int)arrlen(all);
        }
        expand_block(ctx, copy);
        append_block_statements(out, copy);
        if (ctx->bindings != old) arrfree(ctx->bindings);
        ctx->bindings = old;
        ctx->binding_count = old_count;
        arrfree(picked);
        ctx->changed++;
        return;
    }
    if (comptime_error(ctx, stmt)) return;

    switch ((int)stmt->kind) {
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *n = (Iron_ValDecl *)stmt;
            n->type_ann = subst_type(ctx, n->type_ann);
            n->init = expand_expr(ctx, n->init);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *n = (Iron_VarDecl *)stmt;
            n->type_ann = subst_type(ctx, n->type_ann);
            n->init = expand_expr(ctx, n->init);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *n = (Iron_AssignStmt *)stmt;
            n->target = expand_expr(ctx, n->target);
            n->value = expand_expr(ctx, n->value);
            break;
        }
        case IRON_NODE_RETURN:
            ((Iron_ReturnStmt *)stmt)->value =
                expand_expr(ctx, ((Iron_ReturnStmt *)stmt)->value);
            break;
        case IRON_NODE_IF: {
            Iron_IfStmt *n = (Iron_IfStmt *)stmt;
            n->condition = expand_expr(ctx, n->condition);
            expand_block(ctx, n->body);
            for (int i = 0; i < n->elif_count; i++) {
                n->elif_conds[i] = expand_expr(ctx, n->elif_conds[i]);
                expand_block(ctx, n->elif_bodies[i]);
            }
            expand_block(ctx, n->else_body);
            break;
        }
        case IRON_NODE_WHILE:
            ((Iron_WhileStmt *)stmt)->condition =
                expand_expr(ctx, ((Iron_WhileStmt *)stmt)->condition);
            expand_block(ctx, ((Iron_WhileStmt *)stmt)->body);
            break;
        case IRON_NODE_FOR: {
            Iron_ForStmt *n = (Iron_ForStmt *)stmt;
            n->iterable = expand_expr(ctx, n->iterable);
            n->pool_expr = expand_expr(ctx, n->pool_expr);
            expand_block(ctx, n->body);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *n = (Iron_MatchStmt *)stmt;
            n->subject = expand_expr(ctx, n->subject);
            for (int i = 0; i < n->case_count; i++) {
                Iron_MatchCase *mc = (Iron_MatchCase *)n->cases[i];
                mc->pattern = expand_expr(ctx, mc->pattern);
                expand_block(ctx, mc->body);
            }
            expand_block(ctx, n->else_body);
            break;
        }
        case IRON_NODE_BLOCK:
            expand_block(ctx, stmt);
            break;
        default:
            stmt = expand_expr(ctx, stmt);
            break;
    }
    arrput(*out, stmt);
}

static void expand_block(ExpandCtx *ctx, Iron_Node *node) {
    if (!node || node->kind != IRON_NODE_BLOCK) return;
    Iron_Block *block = (Iron_Block *)node;
    Iron_Node **out = NULL;
    for (int i = 0; i < block->stmt_count; i++)
        expand_stmt(ctx, block->stmts[i], &out);
    int n = (int)arrlen(out);
    Iron_Node **copy = n ? (Iron_Node **)iron_arena_arr_new(
        ctx->arena, (size_t)n, sizeof(Iron_Node *)) : NULL;
    if (copy) memcpy(copy, out, sizeof(Iron_Node *) * (size_t)n);
    block->stmts = copy;
    block->stmt_count = copy ? n : 0;
    arrfree(out);
}

int iron_expand_reflection(Iron_Program *program, Iron_Arena *arena,
                           Iron_DiagList *diags) {
    if (!program) return 0;
    int changed = 0;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        Iron_Node *body = NULL;
        const char *current_type = NULL;
        if (d && d->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *f = (Iron_FuncDecl *)d;
            /* Interface templates containing Self wait for synthesis. */
            body = f->body;
        } else if (d && d->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *m = (Iron_MethodDecl *)d;
            body = m->body;
            current_type = m->type_name;
        }
        if (!body) continue;
        ExpandCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.program = program;
        ctx.arena = arena;
        ctx.diags = diags;
        ctx.current_type = current_type;
        expand_block(&ctx, body);
        changed += ctx.changed;
    }
    return changed;
}
