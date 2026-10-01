/* list_ownership.c — no implicit duplication of dynamic lists (#174).
 *
 * A list value is a header {items, count, capacity} owning its buffer.
 * Copying the header into a second owner (a binding, field, element or a
 * returned value) used to share the buffer: growing either copy freed the
 * buffer under the other. Every new owner must therefore receive a fresh
 * list; `a.copy()` and `a.take()` spell out the two ways to get one from an
 * existing list. See list_ownership.h. */

#include "analyzer/list_ownership.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "lexer/lexer.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    Iron_Program  *program;
    Iron_Arena    *arena;
    Iron_DiagList *diags;
} ListOwnCtx;

/* A Map or Set owns its table exactly like a list owns its buffer (#193):
 * the same rules apply, and `holds a list` below covers both. */
static bool is_hash_container(const Iron_Type *t) {
    return t && t->kind == IRON_TYPE_OBJECT && t->object.decl && t->object.decl->name &&
           t->object.elem &&
           (strcmp(t->object.decl->name, "Map") == 0 || strcmp(t->object.decl->name, "Set") == 0);
}

static bool is_dynamic_list(const Iron_Type *t) {
    if (is_hash_container(t)) return true;
    return t && t->kind == IRON_TYPE_ARRAY && t->array.size < 0 &&
           !t->array.is_bounded;
}

/* An object that owns a list, directly or through object fields (rc, heap
 * and pointer fields are handles and do not count): copying it copies the
 * list, so it is never duplicated implicitly either. */
static bool object_holds_list(const Iron_Type *t, int depth) {
    if (!t || t->kind != IRON_TYPE_OBJECT || !t->object.decl || depth > 16) return false;
    Iron_ObjectDecl *od = t->object.decl;
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        Iron_Type *ft = f ? (f->resolved_type ? f->resolved_type : f->field_type_cached) : NULL;
        if (!ft) continue;
        if (is_dynamic_list(ft)) return true;
        if (object_holds_list(ft, depth + 1)) return true;
    }
    return false;
}

static bool owns_list(const Iron_Type *t) {
    return is_dynamic_list(t) || object_holds_list(t, 0);
}

static Iron_Type *node_type(Iron_Node *n) {
    if (!n) return NULL;
    switch ((int)n->kind) {  /* only the ownership-relevant kinds */
        case IRON_NODE_IDENT:        return ((Iron_Ident *)n)->resolved_type;
        case IRON_NODE_FIELD_ACCESS: return ((Iron_FieldAccess *)n)->resolved_type;
        case IRON_NODE_INDEX:        return ((Iron_IndexExpr *)n)->resolved_type;
        default:                     return NULL;
    }
}

/* A list that already has an owner: a variable, parameter, field or
 * element. Literals, calls and method calls (copy / take included) produce
 * fresh lists and are not places. */
static bool is_list_place(Iron_Node *n) {
    if (!n || !owns_list(node_type(n))) return false;
    if (n->kind == IRON_NODE_IDENT) {
        Iron_Symbol *sym = ((Iron_Ident *)n)->resolved_sym;
        return sym && (sym->sym_kind == IRON_SYM_VARIABLE ||
                       sym->sym_kind == IRON_SYM_PARAM ||
                       sym->sym_kind == IRON_SYM_FIELD);
    }
    return n->kind == IRON_NODE_FIELD_ACCESS || n->kind == IRON_NODE_INDEX;
}

static const char *place_text(Iron_Node *n) {
    if (n->kind == IRON_NODE_IDENT) return ((Iron_Ident *)n)->name;
    if (n->kind == IRON_NODE_FIELD_ACCESS) return ((Iron_FieldAccess *)n)->field;
    return NULL;
}

static void require_fresh(ListOwnCtx *c, Iron_Node *value) {
    if (!is_list_place(value)) return;
    const char *name = place_text(value);
    char msg[320], help[320];
    bool is_hash = is_hash_container(node_type(value));
    bool is_obj = !is_dynamic_list(node_type(value));
    if (name && is_hash) {
        const char *what = strcmp(node_type(value)->object.decl->name, "Map") == 0 ? "map" : "set";
        snprintf(msg, sizeof(msg),
                 "%s '%s' cannot be duplicated implicitly", what, name);
        snprintf(help, sizeof(help),
                 "use %s.copy() for an independent %s, %s.take() to move "
                 "its contents out, or share it as rc", name, what, name);
    } else if (name && is_obj) {
        snprintf(msg, sizeof(msg),
                 "'%s' holds a list and cannot be duplicated implicitly", name);
        snprintf(help, sizeof(help),
                 "use %s.copy() for an independent copy, or share it as rc", name);
    } else if (name) {
        snprintf(msg, sizeof(msg),
                 "list '%s' cannot be duplicated implicitly", name);
        snprintf(help, sizeof(help),
                 "use %s.copy() for an independent list, %s.take() to move "
                 "its contents out, or share it as rc", name, name);
    } else {
        snprintf(msg, sizeof(msg), "a list element cannot be duplicated implicitly");
        snprintf(help, sizeof(help),
                 "use .copy() for an independent list or .take() to move "
                 "its contents out");
    }
    iron_diag_emit(c->diags, c->arena, IRON_DIAG_ERROR,
                   IRON_ERR_LIST_IMPLICIT_COPY, value->span,
                   iron_arena_strdup(c->arena, msg, strlen(msg)),
                   iron_arena_strdup(c->arena, help, strlen(help)));
}

/* A top-level declaration (module global) is not an owned local. */
static bool is_top_level_decl(ListOwnCtx *c, Iron_Node *decl) {
    for (int i = 0; i < c->program->decl_count; i++)
        if (c->program->decls[i] == decl) return true;
    return false;
}

/* `return xs` moves a list the function owns: a local val / var. Anything
 * else (a parameter, which is borrowed, a global, a field, an element or a
 * loop variable) is still owned elsewhere. */
static void check_return(ListOwnCtx *c, Iron_Node *value) {
    if (!is_list_place(value)) return;
    if (value->kind == IRON_NODE_IDENT) {
        Iron_Symbol *sym = ((Iron_Ident *)value)->resolved_sym;
        Iron_Node *d = sym ? sym->decl_node : NULL;
        if (sym && sym->sym_kind == IRON_SYM_VARIABLE && d &&
            (d->kind == IRON_NODE_VAL_DECL || d->kind == IRON_NODE_VAR_DECL) &&
            !is_top_level_decl(c, d))
            return;
    }
    require_fresh(c, value);
}

/* Does `type_name` have a user init? Its arguments are then ordinary
 * (borrowed) parameters; without one they become the fields directly. */
static bool type_has_user_init(ListOwnCtx *c, const char *type_name) {
    if (!type_name) return false;
    for (int i = 0; i < c->program->decl_count; i++) {
        Iron_Node *d = c->program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *m = (Iron_MethodDecl *)d;
        if (m->is_init && m->type_name && strcmp(m->type_name, type_name) == 0)
            return true;
    }
    return false;
}

static void check_field_args(ListOwnCtx *c, const char *type_name,
                             Iron_Node **args, int argc) {
    if (type_has_user_init(c, type_name)) return;
    for (int i = 0; i < argc; i++) require_fresh(c, args[i]);
}

static bool visit(Iron_Visitor *v, Iron_Node *n) {
    ListOwnCtx *c = (ListOwnCtx *)v->ctx;
    if (!n) return false;
    switch ((int)n->kind) {  /* only the ownership-relevant kinds */
        case IRON_NODE_METHOD_DECL:
            /* A synthesized pub-field getter returns a view of the field,
             * exactly like reading `o.items`; callers see a place. */
            return !((Iron_MethodDecl *)n)->is_synth_accessor;
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)n;
            if (vd->binding_count == 0) require_fresh(c, vd->init);
            break;
        }
        case IRON_NODE_VAR_DECL:
            require_fresh(c, ((Iron_VarDecl *)n)->init);
            break;
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)n;
            if (as->op == IRON_TOK_ASSIGN) require_fresh(c, as->value);
            break;
        }
        case IRON_NODE_RETURN:
            check_return(c, ((Iron_ReturnStmt *)n)->value);
            break;
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)n;
            for (int i = 0; i < al->element_count; i++)
                require_fresh(c, al->elements[i]);
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)n;
            check_field_args(c, ce->type_name, ce->args, ce->arg_count);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)n;
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *id = (Iron_Ident *)ce->callee;
                if (id->resolved_sym && id->resolved_sym->sym_kind == IRON_SYM_TYPE)
                    check_field_args(c, id->name, ce->args, ce->arg_count);
            }
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            /* push / insert / set store their last argument in the list. */
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)n;
            Iron_Type *rt = mc->object ? node_type(mc->object) : NULL;
            if (!rt && mc->object)
                rt = ((Iron_ExprNode *)mc->object)->resolved_type;
            if (rt && rt->kind == IRON_TYPE_ARRAY && mc->method && mc->arg_count > 0 &&
                (strcmp(mc->method, "push") == 0 || strcmp(mc->method, "insert") == 0 ||
                 strcmp(mc->method, "set") == 0))
                require_fresh(c, mc->args[mc->arg_count - 1]);
            /* A map stores the value of put / get_or's default; a set stores
             * what it adds (#193). */
            if (rt && is_hash_container(rt) && mc->method && mc->arg_count > 0 &&
                (strcmp(mc->method, "put") == 0 || strcmp(mc->method, "get_or") == 0 ||
                 strcmp(mc->method, "add") == 0))
                require_fresh(c, mc->args[mc->arg_count - 1]);
            break;
        }
        default:
            break;
    }
    return true;
}

void iron_list_ownership_check(Iron_Program *program, Iron_Arena *arena,
                               Iron_DiagList *diags,
                               const _Atomic bool *cancel_flag) {
    if (!program || !diags) return;
    ListOwnCtx c = { program, arena, diags };
    Iron_Visitor v = { &c, visit, NULL };
    for (int i = 0; i < program->decl_count; i++) {
        if (cancel_flag && atomic_load(cancel_flag)) return;
        iron_ast_walk(program->decls[i], &v);
    }
}
