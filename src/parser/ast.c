#include "parser/ast.h"
#include <stddef.h>
#include <string.h>

/* ── Node kind names ─────────────────────────────────────────────────────── */

static const char *s_node_kind_names[IRON_NODE_COUNT] = {
    [IRON_NODE_PROGRAM]         = "Program",
    [IRON_NODE_IMPORT_DECL]     = "ImportDecl",
    [IRON_NODE_OBJECT_DECL]     = "ObjectDecl",
    [IRON_NODE_INTERFACE_DECL]  = "InterfaceDecl",
    [IRON_NODE_ENUM_DECL]       = "EnumDecl",
    [IRON_NODE_FUNC_DECL]       = "FuncDecl",
    [IRON_NODE_METHOD_DECL]     = "MethodDecl",
    [IRON_NODE_VAL_DECL]        = "ValDecl",
    [IRON_NODE_VAR_DECL]        = "VarDecl",
    [IRON_NODE_ASSIGN]          = "AssignStmt",
    [IRON_NODE_RETURN]          = "ReturnStmt",
    [IRON_NODE_IF]              = "IfStmt",
    [IRON_NODE_WHILE]           = "WhileStmt",
    [IRON_NODE_FOR]             = "ForStmt",
    [IRON_NODE_MATCH]           = "MatchStmt",
    [IRON_NODE_DEFER]           = "DeferStmt",
    [IRON_NODE_BREAK]           = "BreakStmt",
    [IRON_NODE_CONTINUE]        = "ContinueStmt",
    [IRON_NODE_FREE]            = "FreeStmt",
    [IRON_NODE_LEAK]            = "LeakStmt",
    [IRON_NODE_SPAWN]           = "SpawnStmt",
    [IRON_NODE_BLOCK]           = "Block",
    [IRON_NODE_IN_ARENA]        = "InArenaBlock",  /* Phase 28 ARENA-02 */
    [IRON_NODE_INT_LIT]         = "IntLit",
    [IRON_NODE_FLOAT_LIT]       = "FloatLit",
    [IRON_NODE_STRING_LIT]      = "StringLit",
    [IRON_NODE_INTERP_STRING]   = "InterpString",
    [IRON_NODE_BOOL_LIT]        = "BoolLit",
    [IRON_NODE_NULL_LIT]        = "NullLit",
    [IRON_NODE_IDENT]           = "Ident",
    [IRON_NODE_BINARY]          = "BinaryExpr",
    [IRON_NODE_UNARY]           = "UnaryExpr",
    [IRON_NODE_CALL]            = "CallExpr",
    [IRON_NODE_METHOD_CALL]     = "MethodCallExpr",
    [IRON_NODE_FIELD_ACCESS]    = "FieldAccess",
    [IRON_NODE_INDEX]           = "IndexExpr",
    [IRON_NODE_SLICE]           = "SliceExpr",
    [IRON_NODE_LAMBDA]          = "LambdaExpr",
    [IRON_NODE_HEAP]            = "HeapExpr",
    [IRON_NODE_RC]              = "RcExpr",
    [IRON_NODE_WEAK_RC_NULL]    = "WeakRcNullExpr",  /* Phase 27 POL-08 */
    [IRON_NODE_COMPTIME]        = "ComptimeExpr",
    [IRON_NODE_IS]              = "IsExpr",
    [IRON_NODE_CONSTRUCT]       = "ConstructExpr",
    [IRON_NODE_ARRAY_LIT]       = "ArrayLit",
    [IRON_NODE_AWAIT]           = "AwaitExpr",
    [IRON_NODE_ERROR]           = "ErrorNode",
    [IRON_NODE_PARAM]           = "Param",
    [IRON_NODE_FIELD]           = "Field",
    [IRON_NODE_MATCH_CASE]      = "MatchCase",
    [IRON_NODE_ENUM_VARIANT]    = "EnumVariant",
    [IRON_NODE_TYPE_ANNOTATION] = "TypeAnnotation",
    [IRON_NODE_PATTERN]         = "Pattern",
    [IRON_NODE_ENUM_CONSTRUCT]  = "EnumConstruct",
};

const char *iron_node_kind_str(Iron_NodeKind kind) {
    if (kind < 0 || kind >= IRON_NODE_COUNT) return "<unknown>";
    const char *s = s_node_kind_names[kind];
    return s ? s : "<unknown>";
}

/* ── Walk helpers ────────────────────────────────────────────────────────── */

/* Dispatch visit to a single child node (may be NULL). */
static void walk_child(Iron_Node *child, Iron_Visitor *v) {
    if (child) iron_ast_walk(child, v);
}

/* Dispatch visit to an array of children. */
static void walk_children(Iron_Node **children, int count, Iron_Visitor *v) {
    for (int i = 0; i < count; i++) {
        if (children[i]) iron_ast_walk(children[i], v);
    }
}

/* ── Main walk ───────────────────────────────────────────────────────────── */

void iron_ast_walk(Iron_Node *root, Iron_Visitor *v) {
    if (!root || !v || !v->visit_node) return;

    bool recurse = v->visit_node(v, root);
    if (!recurse) return;

    switch (root->kind) {
        case IRON_NODE_PROGRAM: {
            Iron_Program *n = (Iron_Program *)root;
            walk_children(n->decls, n->decl_count, v);
            break;
        }
        case IRON_NODE_IMPORT_DECL:
        case IRON_NODE_BREAK:
        case IRON_NODE_CONTINUE:
            /* leaf — no child nodes */
            break;
        case IRON_NODE_OBJECT_DECL: {
            Iron_ObjectDecl *n = (Iron_ObjectDecl *)root;
            walk_children(n->generic_params, n->generic_param_count, v);
            walk_children(n->fields, n->field_count, v);
            break;
        }
        case IRON_NODE_INTERFACE_DECL: {
            Iron_InterfaceDecl *n = (Iron_InterfaceDecl *)root;
            walk_children(n->method_sigs, n->method_count, v);
            break;
        }
        case IRON_NODE_ENUM_DECL: {
            Iron_EnumDecl *n = (Iron_EnumDecl *)root;
            walk_children(n->variants, n->variant_count, v);
            break;
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *n = (Iron_FuncDecl *)root;
            walk_children(n->generic_params, n->generic_param_count, v);
            walk_children(n->params, n->param_count, v);
            walk_child(n->return_type, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *n = (Iron_MethodDecl *)root;
            walk_children(n->generic_params, n->generic_param_count, v);
            walk_children(n->params, n->param_count, v);
            walk_child(n->return_type, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_PARAM: {
            Iron_Param *n = (Iron_Param *)root;
            walk_child(n->type_ann, v);
            break;
        }
        case IRON_NODE_FIELD: {
            Iron_Field *n = (Iron_Field *)root;
            walk_child(n->type_ann, v);
            break;
        }
        case IRON_NODE_ENUM_VARIANT: {
            Iron_EnumVariant *n = (Iron_EnumVariant *)root;
            walk_children(n->payload_type_anns, n->payload_count, v);
            break;
        }
        case IRON_NODE_TYPE_ANNOTATION: {
            Iron_TypeAnnotation *n = (Iron_TypeAnnotation *)root;
            walk_children(n->generic_args, n->generic_arg_count, v);
            walk_child(n->array_size, v);
            break;
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *n = (Iron_ValDecl *)root;
            walk_child(n->type_ann, v);
            walk_child(n->init, v);
            break;
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *n = (Iron_VarDecl *)root;
            walk_child(n->type_ann, v);
            walk_child(n->init, v);
            break;
        }
        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *n = (Iron_AssignStmt *)root;
            walk_child(n->target, v);
            walk_child(n->value, v);
            break;
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *n = (Iron_ReturnStmt *)root;
            walk_child(n->value, v);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *n = (Iron_IfStmt *)root;
            walk_child(n->condition, v);
            walk_child(n->body, v);
            walk_children(n->elif_conds, n->elif_count, v);
            walk_children(n->elif_bodies, n->elif_count, v);
            walk_child(n->else_body, v);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *n = (Iron_WhileStmt *)root;
            walk_child(n->condition, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *n = (Iron_ForStmt *)root;
            walk_child(n->iterable, v);
            walk_child(n->pool_expr, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_IN_ARENA: {  /* Phase 28 ARENA-02 (Plan 28-03) */
            Iron_InArenaBlock *n = (Iron_InArenaBlock *)root;
            walk_child(n->arena_expr, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *n = (Iron_MatchStmt *)root;
            walk_child(n->subject, v);
            walk_children(n->cases, n->case_count, v);
            walk_child(n->else_body, v);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *n = (Iron_MatchCase *)root;
            walk_child(n->pattern, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_DEFER: {
            Iron_DeferStmt *n = (Iron_DeferStmt *)root;
            walk_child(n->expr, v);
            break;
        }
        case IRON_NODE_FREE: {
            Iron_FreeStmt *n = (Iron_FreeStmt *)root;
            walk_child(n->expr, v);
            break;
        }
        case IRON_NODE_LEAK: {
            Iron_LeakStmt *n = (Iron_LeakStmt *)root;
            walk_child(n->expr, v);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *n = (Iron_SpawnStmt *)root;
            walk_child(n->pool_expr, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *n = (Iron_Block *)root;
            walk_children(n->stmts, n->stmt_count, v);
            break;
        }
        /* Literal leaves */
        case IRON_NODE_INT_LIT:
        case IRON_NODE_FLOAT_LIT:
        case IRON_NODE_STRING_LIT:
        case IRON_NODE_BOOL_LIT:
        case IRON_NODE_NULL_LIT:
        case IRON_NODE_IDENT:
        case IRON_NODE_ERROR:
            break;
        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *n = (Iron_InterpString *)root;
            walk_children(n->parts, n->part_count, v);
            break;
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *n = (Iron_BinaryExpr *)root;
            walk_child(n->left, v);
            walk_child(n->right, v);
            break;
        }
        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *n = (Iron_UnaryExpr *)root;
            walk_child(n->operand, v);
            break;
        }
        case IRON_NODE_CALL: {
            Iron_CallExpr *n = (Iron_CallExpr *)root;
            walk_child(n->callee, v);
            walk_children(n->args, n->arg_count, v);
            break;
        }
        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *n = (Iron_MethodCallExpr *)root;
            walk_child(n->object, v);
            walk_children(n->args, n->arg_count, v);
            break;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *n = (Iron_FieldAccess *)root;
            walk_child(n->object, v);
            break;
        }
        case IRON_NODE_INDEX: {
            Iron_IndexExpr *n = (Iron_IndexExpr *)root;
            walk_child(n->object, v);
            walk_child(n->index, v);
            break;
        }
        case IRON_NODE_SLICE: {
            Iron_SliceExpr *n = (Iron_SliceExpr *)root;
            walk_child(n->object, v);
            walk_child(n->start, v);
            walk_child(n->end, v);
            break;
        }
        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *n = (Iron_LambdaExpr *)root;
            walk_children(n->params, n->param_count, v);
            walk_child(n->return_type, v);
            walk_child(n->body, v);
            break;
        }
        case IRON_NODE_HEAP: {
            Iron_HeapExpr *n = (Iron_HeapExpr *)root;
            walk_child(n->inner, v);
            break;
        }
        case IRON_NODE_RC: {
            Iron_RcExpr *n = (Iron_RcExpr *)root;
            walk_child(n->inner, v);
            break;
        }
        case IRON_NODE_WEAK_RC_NULL:
            /* Phase 27 POL-08: leaf node — no children to walk. */
            break;
        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *n = (Iron_ComptimeExpr *)root;
            walk_child(n->inner, v);
            break;
        }
        case IRON_NODE_IS: {
            Iron_IsExpr *n = (Iron_IsExpr *)root;
            walk_child(n->expr, v);
            break;
        }
        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *n = (Iron_AwaitExpr *)root;
            walk_child(n->handle, v);
            break;
        }
        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *n = (Iron_ConstructExpr *)root;
            walk_children(n->args, n->arg_count, v);
            walk_children(n->generic_args, n->generic_arg_count, v);
            break;
        }
        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *n = (Iron_ArrayLit *)root;
            walk_child(n->type_ann, v);
            walk_child(n->size, v);
            walk_children(n->elements, n->element_count, v);
            break;
        }
        case IRON_NODE_PATTERN: {
            Iron_Pattern *n = (Iron_Pattern *)root;
            walk_children(n->nested_patterns, n->binding_count, v);
            break;
        }
        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *n = (Iron_EnumConstruct *)root;
            walk_children(n->args, n->arg_count, v);
            break;
        }
        case IRON_NODE_COUNT:
            break;
    }

    if (v->post_visit) v->post_visit(v, root);
}

bool iron_node_doc(const Iron_Node *n, const char **doc,
                   const Iron_Directive **dirs, int *count) {
    const char *d = NULL;
    const Iron_Directive *x = NULL;
    int c = 0;
    bool ok = true;
    switch (n ? (int)n->kind : -1) {
#define TAKE(T) { const T *p = (const T *)n; d = p->doc_comment; \
                  x = p->directives; c = p->directive_count; break; }
        case IRON_NODE_IMPORT_DECL:    TAKE(Iron_ImportDecl)
        case IRON_NODE_OBJECT_DECL:    TAKE(Iron_ObjectDecl)
        case IRON_NODE_INTERFACE_DECL: TAKE(Iron_InterfaceDecl)
        case IRON_NODE_ENUM_DECL:      TAKE(Iron_EnumDecl)
        case IRON_NODE_FUNC_DECL:      TAKE(Iron_FuncDecl)
        case IRON_NODE_METHOD_DECL:    TAKE(Iron_MethodDecl)
        case IRON_NODE_VAL_DECL:       TAKE(Iron_ValDecl)
        case IRON_NODE_VAR_DECL:       TAKE(Iron_VarDecl)
        case IRON_NODE_FIELD:          TAKE(Iron_Field)
        case IRON_NODE_ENUM_VARIANT:   TAKE(Iron_EnumVariant)
#undef TAKE
        default: ok = false; break;
    }
    if (doc) *doc = d;
    if (dirs) *dirs = x;
    if (count) *count = c;
    return ok;
}

bool iron_type_ann_mentions_self(Iron_Node *n) {
    if (!n || n->kind != IRON_NODE_TYPE_ANNOTATION) return false;
    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)n;
    if (ta->is_self_type || (ta->name && strcmp(ta->name, "Self") == 0)) return true;
    for (int i = 0; i < ta->generic_arg_count; i++)
        if (iron_type_ann_mentions_self(ta->generic_args[i])) return true;
    for (int i = 0; i < ta->func_param_count; i++)
        if (iron_type_ann_mentions_self(ta->func_params[i])) return true;
    for (int i = 0; i < ta->tuple_elem_count; i++)
        if (iron_type_ann_mentions_self(ta->tuple_elems[i])) return true;
    return iron_type_ann_mentions_self(ta->func_return) ||
           iron_type_ann_mentions_self(ta->pointer_pointee) ||
           iron_type_ann_mentions_self(ta->weak_rc_inner) ||
           iron_type_ann_mentions_self(ta->rc_inner) ||
           iron_type_ann_mentions_self(ta->array_elem_ann);
}

bool iron_iface_sig_dispatchable(const Iron_FuncDecl *sig) {
    if (!sig || sig->is_static) return false;
    for (int i = 0; i < sig->param_count; i++) {
        Iron_Param *p = (Iron_Param *)sig->params[i];
        if (p && p->kind == IRON_NODE_PARAM && iron_type_ann_mentions_self(p->type_ann))
            return false;
    }
    Iron_Node *rt = sig->return_type;
    if (rt && rt->kind == IRON_NODE_TYPE_ANNOTATION && iron_type_ann_mentions_self(rt)) {
        Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)rt;
        bool plain = (ta->is_self_type || (ta->name && strcmp(ta->name, "Self") == 0)) &&
                     !ta->is_nullable && !ta->is_array && !ta->is_pointer && !ta->is_rc &&
                     !ta->is_weak_rc && ta->generic_arg_count == 0;
        if (!plain) return false;
    }
    return true;
}
