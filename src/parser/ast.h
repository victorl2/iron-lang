#ifndef IRON_AST_H
#define IRON_AST_H

#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include <stdbool.h>
#include <stdint.h>

/* Forward declarations for semantic annotation fields.
 * Do NOT include analyzer headers from ast.h — the analyzer includes ast.h. */
struct Iron_Symbol;
struct Iron_Type;

/* Capture analysis annotation — set by capture.c pass */
typedef struct {
    const char      *name;        /* original Iron variable name */
    struct Iron_Type *type;       /* resolved type from symbol table */
    bool             is_mutable;  /* true = var capture (by pointer), false = val (by value) */
} Iron_CaptureEntry;

/* ── Node kinds ──────────────────────────────────────────────────────────── */

typedef enum {
    /* Program root */
    IRON_NODE_PROGRAM,

    /* Top-level declarations */
    IRON_NODE_IMPORT_DECL,
    IRON_NODE_OBJECT_DECL,
    IRON_NODE_INTERFACE_DECL,
    IRON_NODE_ENUM_DECL,
    IRON_NODE_FUNC_DECL,
    IRON_NODE_METHOD_DECL,

    /* Statements */
    IRON_NODE_VAL_DECL,
    IRON_NODE_VAR_DECL,
    IRON_NODE_ASSIGN,
    IRON_NODE_RETURN,
    IRON_NODE_IF,
    IRON_NODE_WHILE,
    IRON_NODE_FOR,
    IRON_NODE_MATCH,
    IRON_NODE_DEFER,
    IRON_NODE_FREE,
    IRON_NODE_LEAK,
    IRON_NODE_SPAWN,
    IRON_NODE_BLOCK,

    /* Expressions */
    IRON_NODE_INT_LIT,
    IRON_NODE_FLOAT_LIT,
    IRON_NODE_STRING_LIT,
    IRON_NODE_INTERP_STRING,
    IRON_NODE_BOOL_LIT,
    IRON_NODE_NULL_LIT,
    IRON_NODE_IDENT,
    IRON_NODE_BINARY,
    IRON_NODE_UNARY,
    IRON_NODE_CALL,
    IRON_NODE_METHOD_CALL,
    IRON_NODE_FIELD_ACCESS,
    IRON_NODE_INDEX,
    IRON_NODE_SLICE,
    IRON_NODE_LAMBDA,
    IRON_NODE_HEAP,
    IRON_NODE_RC,
    IRON_NODE_COMPTIME,
    IRON_NODE_IS,
    IRON_NODE_CONSTRUCT,
    IRON_NODE_ARRAY_LIT,
    IRON_NODE_AWAIT,

    /* Error recovery */
    IRON_NODE_ERROR,

    /* Structural helpers */
    IRON_NODE_PARAM,
    IRON_NODE_FIELD,
    IRON_NODE_MATCH_CASE,
    IRON_NODE_ENUM_VARIANT,
    IRON_NODE_TYPE_ANNOTATION,

    IRON_NODE_COUNT
} Iron_NodeKind;

/* ── Base node ───────────────────────────────────────────────────────────── */

/* All AST nodes embed Iron_Span span + Iron_NodeKind kind as their first two
 * fields so a pointer to any node can be safely cast to Iron_Node *. */
typedef struct Iron_Node {
    Iron_Span     span;
    Iron_NodeKind kind;
} Iron_Node;

/* ── Forward-declare token kind for operators stored in nodes ────────────── */
/* We need Iron_TokenKind from lexer.h but avoid a circular include.
 * The token kind values fit in an int; use int here and cast at use sites. */
typedef int Iron_OpKind;  /* stores Iron_TokenKind values for operators */

/* ── Top-level declarations ──────────────────────────────────────────────── */

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_PROGRAM */
    Iron_Node   **decls;
    int           decl_count;
} Iron_Program;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_IMPORT_DECL */
    const char   *path;
    const char   *alias;  /* NULL if no alias */
} Iron_ImportDecl;

typedef struct Iron_ObjectDecl {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_OBJECT_DECL */
    const char   *name;
    Iron_Node   **fields;
    int           field_count;
    const char   *extends_name;      /* NULL if none */
    const char  **implements_names;  /* array of name strings */
    int           implements_count;
    Iron_Node   **generic_params;
    int           generic_param_count;
} Iron_ObjectDecl;

typedef struct Iron_InterfaceDecl {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_INTERFACE_DECL */
    const char   *name;
    Iron_Node   **method_sigs;
    int           method_count;
} Iron_InterfaceDecl;

typedef struct Iron_EnumDecl {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ENUM_DECL */
    const char   *name;
    Iron_Node   **variants;
    int           variant_count;
} Iron_EnumDecl;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_FUNC_DECL */
    const char        *name;
    Iron_Node        **params;
    int                param_count;
    Iron_Node         *return_type;  /* NULL if void */
    Iron_Node         *body;         /* Iron_Block; NULL for extern funcs */
    bool               is_private;
    bool               is_extern;      /* true for extern func declarations */
    const char        *extern_c_name;  /* C function name, e.g. "InitWindow"; NULL for non-extern */
    Iron_Node        **generic_params;
    int                generic_param_count;
    struct Iron_Type  *resolved_return_type;  /* set by type checker */
} Iron_FuncDecl;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_METHOD_DECL */
    const char        *type_name;
    const char        *method_name;
    Iron_Node        **params;
    int                param_count;
    Iron_Node         *return_type;  /* NULL if void */
    Iron_Node         *body;
    bool               is_private;
    Iron_Node        **generic_params;
    int                generic_param_count;
    struct Iron_Type  *resolved_return_type;  /* set by type checker */
    struct Iron_Symbol *owner_sym;             /* set by resolver: the owning type */
} Iron_MethodDecl;

/* ── Helper node types ───────────────────────────────────────────────────── */

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_PARAM */
    const char   *name;
    Iron_Node    *type_ann;  /* NULL if inferred */
    bool          is_var;
} Iron_Param;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_FIELD */
    const char   *name;
    Iron_Node    *type_ann;
    bool          is_var;
} Iron_Field;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ENUM_VARIANT */
    const char   *name;
    bool          has_explicit_value;  /* true when variant has = N */
    int           explicit_value;      /* only valid when has_explicit_value */
} Iron_EnumVariant;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_TYPE_ANNOTATION */
    const char   *name;
    bool          is_nullable;
    Iron_Node   **generic_args;
    int           generic_arg_count;
    bool          is_array;
    Iron_Node    *array_size;  /* NULL if dynamic/no size */
    /* Phase 33: function type annotations — func(T, U) -> R */
    bool          is_func;
    Iron_Node   **func_params;      /* array of Iron_TypeAnnotation* for param types */
    int           func_param_count;
    Iron_Node    *func_return;      /* return type annotation, NULL means void */
} Iron_TypeAnnotation;

/* ── Statements ──────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_VAL_DECL */
    const char        *name;
    Iron_Node         *type_ann;  /* NULL if inferred */
    Iron_Node         *init;
    struct Iron_Type  *declared_type;  /* set by type checker */
} Iron_ValDecl;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_VAR_DECL */
    const char        *name;
    Iron_Node         *type_ann;  /* NULL if inferred */
    Iron_Node         *init;
    struct Iron_Type  *declared_type;  /* set by type checker */
} Iron_VarDecl;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ASSIGN */
    Iron_Node    *target;
    Iron_Node    *value;
    Iron_OpKind   op;  /* ASSIGN, PLUS_ASSIGN, etc. */
} Iron_AssignStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_RETURN */
    Iron_Node    *value;  /* NULL if bare return */
} Iron_ReturnStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;        /* IRON_NODE_IF */
    Iron_Node    *condition;
    Iron_Node    *body;
    Iron_Node   **elif_conds;  /* stb_ds array */
    Iron_Node   **elif_bodies; /* stb_ds array */
    int           elif_count;
    Iron_Node    *else_body;   /* NULL if no else */
} Iron_IfStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;      /* IRON_NODE_WHILE */
    Iron_Node    *condition;
    Iron_Node    *body;
} Iron_WhileStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;        /* IRON_NODE_FOR */
    const char   *var_name;
    Iron_Node    *iterable;
    Iron_Node    *body;
    bool          is_parallel;
    Iron_Node    *pool_expr;   /* NULL if default pool */
} Iron_ForStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;       /* IRON_NODE_MATCH */
    Iron_Node    *subject;
    Iron_Node   **cases;
    int           case_count;
    Iron_Node    *else_body;  /* NULL if no else */
} Iron_MatchStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;     /* IRON_NODE_MATCH_CASE */
    Iron_Node    *pattern;
    Iron_Node    *body;
} Iron_MatchCase;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_DEFER */
    Iron_Node    *expr;
} Iron_DeferStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_FREE */
    Iron_Node    *expr;
} Iron_FreeStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_LEAK */
    Iron_Node    *expr;
} Iron_LeakStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;          /* IRON_NODE_SPAWN */
    const char   *name;
    Iron_Node    *pool_expr;     /* NULL if default */
    Iron_Node    *body;
    const char   *handle_name;  /* NULL if no handle */
} Iron_SpawnStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_BLOCK */
    Iron_Node   **stmts;
    int           stmt_count;
} Iron_Block;

/* ── Expressions ─────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_INT_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *value;
} Iron_IntLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_FLOAT_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *value;
} Iron_FloatLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_STRING_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *value;
} Iron_StringLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;       /* IRON_NODE_INTERP_STRING */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node        **parts;      /* alternating string lit and expr */
    int                part_count;
} Iron_InterpString;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_BOOL_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    bool               value;
} Iron_BoolLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_NULL_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
} Iron_NullLit;

typedef struct {
    Iron_Span           span;
    Iron_NodeKind       kind;  /* IRON_NODE_IDENT */
    struct Iron_Type   *resolved_type;  /* set by type checker */
    const char         *name;
    struct Iron_Symbol *resolved_sym;   /* set by resolver; NULL = unresolved */
} Iron_Ident;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_BINARY */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *left;
    Iron_OpKind        op;
    Iron_Node         *right;
} Iron_BinaryExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;     /* IRON_NODE_UNARY */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_OpKind        op;
    Iron_Node         *operand;
} Iron_UnaryExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;     /* IRON_NODE_CALL */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *callee;
    Iron_Node        **args;
    int                arg_count;
    bool               is_primitive_cast; /* true when Float(x), Int(x), etc. */
} Iron_CallExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;        /* IRON_NODE_METHOD_CALL */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    const char        *method;
    Iron_Node        **args;
    int                arg_count;
} Iron_MethodCallExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_FIELD_ACCESS */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    const char        *field;
} Iron_FieldAccess;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_INDEX */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    Iron_Node         *index;
} Iron_IndexExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_SLICE */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    Iron_Node         *start;  /* NULL if omitted */
    Iron_Node         *end;    /* NULL if omitted */
} Iron_SliceExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;         /* IRON_NODE_LAMBDA */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node        **params;
    int                param_count;
    Iron_Node         *return_type;  /* NULL if inferred */
    Iron_Node         *body;
    Iron_CaptureEntry *captures;      /* set by capture analysis; NULL before analysis */
    int                capture_count; /* 0 for non-capturing lambdas */
} Iron_LambdaExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_HEAP */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *inner;
    bool               auto_free;  /* set by escape analyzer */
    bool               escapes;    /* set by escape analyzer */
} Iron_HeapExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_RC */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *inner;
} Iron_RcExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_COMPTIME */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *inner;
} Iron_ComptimeExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;       /* IRON_NODE_IS */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *expr;
    const char        *type_name;
} Iron_IsExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;            /* IRON_NODE_AWAIT */
    struct Iron_Type  *resolved_type;   /* set by type checker */
    Iron_Node         *handle;
} Iron_AwaitExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;           /* IRON_NODE_CONSTRUCT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *type_name;
    Iron_Node        **args;
    int                arg_count;
    Iron_Node        **generic_args;
    int                generic_arg_count;
} Iron_ConstructExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;          /* IRON_NODE_ARRAY_LIT */
    struct Iron_Type  *resolved_type; /* set by type checker */
    Iron_Node         *type_ann;      /* NULL if inferred */
    Iron_Node         *size;          /* NULL if dynamic */
    Iron_Node        **elements;
    int                element_count;
} Iron_ArrayLit;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ERROR */
} Iron_ErrorNode;

/* ── Visitor pattern ─────────────────────────────────────────────────────── */

typedef struct Iron_Visitor {
    void *ctx;
    /* Called before visiting children. Return true to recurse into children. */
    bool (*visit_node)(struct Iron_Visitor *v, Iron_Node *node);
    /* Called after visiting children. May be NULL. */
    void (*post_visit)(struct Iron_Visitor *v, Iron_Node *node);
} Iron_Visitor;

/* Walk the AST rooted at `root`, calling v->visit_node on each node.
 * If visit_node returns true, children are visited recursively.
 * After children, v->post_visit is called (if non-NULL). */
void iron_ast_walk(Iron_Node *root, Iron_Visitor *v);

/* Return a human-readable name for the node kind. */
const char *iron_node_kind_str(Iron_NodeKind kind);

#endif /* IRON_AST_H */
