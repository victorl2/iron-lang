#ifndef IRON_HIR_H
#define IRON_HIR_H

#include "diagnostics/diagnostics.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Variable ID ─────────────────────────────────────────────────────────── */

typedef uint32_t IronHIR_VarId;

#define IRON_HIR_VAR_INVALID 0

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct IronHIR_Stmt   IronHIR_Stmt;
typedef struct IronHIR_Expr   IronHIR_Expr;
typedef struct IronHIR_Block  IronHIR_Block;
typedef struct IronHIR_Func   IronHIR_Func;
typedef struct IronHIR_Module IronHIR_Module;

/* ── Statement kind enum ─────────────────────────────────────────────────── */

typedef enum {
    IRON_HIR_STMT_LET,        /* val/var binding                   */
    IRON_HIR_STMT_ASSIGN,     /* lvalue = expr                     */
    IRON_HIR_STMT_IF,         /* if/else tree node                 */
    IRON_HIR_STMT_WHILE,      /* while loop                        */
    IRON_HIR_STMT_FOR,        /* for-in loop                       */
    IRON_HIR_STMT_MATCH,      /* match statement                   */
    IRON_HIR_STMT_RETURN,     /* return statement                  */
    IRON_HIR_STMT_DEFER,      /* defer statement                   */
    IRON_HIR_STMT_BLOCK,      /* nested block                      */
    IRON_HIR_STMT_EXPR,       /* expression statement              */
    IRON_HIR_STMT_FREE,       /* explicit memory free              */
    IRON_HIR_STMT_SPAWN,      /* spawn concurrent task             */
    IRON_HIR_STMT_LEAK        /* intentional memory leak           */
} IronHIR_StmtKind;

/* ── Expression kind enum ────────────────────────────────────────────────── */

typedef enum {
    /* Literals */
    IRON_HIR_EXPR_INT_LIT,       /* integer literal                */
    IRON_HIR_EXPR_FLOAT_LIT,     /* float literal                  */
    IRON_HIR_EXPR_STRING_LIT,    /* string literal                 */
    IRON_HIR_EXPR_INTERP_STRING, /* string interpolation           */
    IRON_HIR_EXPR_BOOL_LIT,      /* bool literal                   */
    IRON_HIR_EXPR_NULL_LIT,      /* null literal                   */

    /* Variables and operators */
    IRON_HIR_EXPR_IDENT,         /* variable reference             */
    IRON_HIR_EXPR_BINOP,         /* binary operation               */
    IRON_HIR_EXPR_UNOP,          /* unary operation                */

    /* Calls and member access */
    IRON_HIR_EXPR_CALL,          /* function call                  */
    IRON_HIR_EXPR_METHOD_CALL,   /* method call: obj.method(args)  */
    IRON_HIR_EXPR_FIELD_ACCESS,  /* field access: obj.field        */
    IRON_HIR_EXPR_INDEX,         /* array index: arr[idx]          */
    IRON_HIR_EXPR_SLICE,         /* slice: arr[start..end]         */

    /* Higher-order / concurrency */
    IRON_HIR_EXPR_CLOSURE,       /* closure / lambda               */
    IRON_HIR_EXPR_PARALLEL_FOR,  /* parallel-for expression        */
    IRON_HIR_EXPR_COMPTIME,      /* comptime expression            */

    /* Memory management */
    IRON_HIR_EXPR_HEAP,          /* heap allocation                */
    IRON_HIR_EXPR_RC,            /* reference-counted allocation   */

    /* Construction and type operations */
    IRON_HIR_EXPR_CONSTRUCT,     /* struct/object construction     */
    IRON_HIR_EXPR_ARRAY_LIT,     /* array literal                  */
    IRON_HIR_EXPR_AWAIT,         /* await expression               */
    IRON_HIR_EXPR_CAST,          /* type cast                      */
    IRON_HIR_EXPR_IS_NULL,       /* null check                     */
    IRON_HIR_EXPR_IS_NOT_NULL,   /* non-null check                 */
    IRON_HIR_EXPR_FUNC_REF,      /* function reference             */
    IRON_HIR_EXPR_IS             /* type test / pattern match check */
} IronHIR_ExprKind;

/* ── Binary operator ────────────────────────────────────────────────────── */

typedef enum {
    IRON_HIR_BINOP_ADD,   /* +  */
    IRON_HIR_BINOP_SUB,   /* -  */
    IRON_HIR_BINOP_MUL,   /* *  */
    IRON_HIR_BINOP_DIV,   /* /  */
    IRON_HIR_BINOP_MOD,   /* %  */
    IRON_HIR_BINOP_EQ,    /* == */
    IRON_HIR_BINOP_NEQ,   /* != */
    IRON_HIR_BINOP_LT,    /* <  */
    IRON_HIR_BINOP_LTE,   /* <= */
    IRON_HIR_BINOP_GT,    /* >  */
    IRON_HIR_BINOP_GTE,   /* >= */
    IRON_HIR_BINOP_AND,   /* && */
    IRON_HIR_BINOP_OR     /* || */
} IronHIR_BinOp;

/* ── Unary operator ─────────────────────────────────────────────────────── */

typedef enum {
    IRON_HIR_UNOP_NEG,  /* -x  */
    IRON_HIR_UNOP_NOT   /* !x  */
} IronHIR_UnOp;

/* ── Helper structs ──────────────────────────────────────────────────────── */

/* Parameter declaration */
typedef struct {
    IronHIR_VarId  var_id;
    const char    *name;
    Iron_Type     *type;
} IronHIR_Param;

/* Variable info stored in name_table */
typedef struct {
    IronHIR_VarId  id;
    const char    *name;
    Iron_Type     *type;
    bool           is_mutable;
} IronHIR_VarInfo;

/* Match arm: pattern + guard + body block */
typedef struct {
    IronHIR_Expr  *pattern;  /* pattern expression (ident, literal, etc.) */
    IronHIR_Expr  *guard;    /* optional guard expr; NULL if none */
    IronHIR_Block *body;
} IronHIR_MatchArm;

/* A sequential block of statements */
struct IronHIR_Block {
    IronHIR_Stmt **stmts;       /* stb_ds array */
    int            stmt_count;
};

/* ── Statement (tagged union) ────────────────────────────────────────────── */

struct IronHIR_Stmt {
    IronHIR_StmtKind kind;
    Iron_Span        span;

    union {
        /* IRON_HIR_STMT_LET */
        struct {
            IronHIR_VarId  var_id;
            Iron_Type     *type;       /* declared type; NULL if inferred */
            IronHIR_Expr  *init;       /* initializer; NULL if uninitialized */
            bool           is_mutable; /* var vs val */
        } let;

        /* IRON_HIR_STMT_ASSIGN */
        struct {
            IronHIR_Expr *target;
            IronHIR_Expr *value;
        } assign;

        /* IRON_HIR_STMT_IF */
        struct {
            IronHIR_Expr  *condition;
            IronHIR_Block *then_body;
            IronHIR_Block *else_body;  /* NULL if no else */
        } if_else;

        /* IRON_HIR_STMT_WHILE */
        struct {
            IronHIR_Expr  *condition;
            IronHIR_Block *body;
        } while_loop;

        /* IRON_HIR_STMT_FOR */
        struct {
            IronHIR_VarId  var_id;
            IronHIR_Expr  *iterable;
            IronHIR_Block *body;
        } for_loop;

        /* IRON_HIR_STMT_MATCH */
        struct {
            IronHIR_Expr     *scrutinee;
            IronHIR_MatchArm *arms;       /* stb_ds array */
            int               arm_count;
        } match_stmt;

        /* IRON_HIR_STMT_RETURN */
        struct {
            IronHIR_Expr *value;  /* NULL for void return */
        } return_stmt;

        /* IRON_HIR_STMT_DEFER */
        struct {
            IronHIR_Block *body;
        } defer;

        /* IRON_HIR_STMT_BLOCK */
        struct {
            IronHIR_Block *block;
        } block;

        /* IRON_HIR_STMT_EXPR */
        struct {
            IronHIR_Expr *expr;
        } expr_stmt;

        /* IRON_HIR_STMT_FREE */
        struct {
            IronHIR_Expr *value;
        } free_stmt;

        /* IRON_HIR_STMT_SPAWN */
        struct {
            const char    *handle_name;
            IronHIR_Block *body;
            const char    *lifted_name; /* assigned top-level name e.g. "__spawn_0" */
            IronHIR_VarId  handle_var;  /* var ID for the handle (0 = no binding) */
        } spawn;

        /* IRON_HIR_STMT_LEAK */
        struct {
            IronHIR_Expr *value;
        } leak;
    };
};

/* ── Expression (tagged union) ───────────────────────────────────────────── */

struct IronHIR_Expr {
    IronHIR_ExprKind kind;
    Iron_Span        span;
    Iron_Type       *type;  /* resolved type; NULL before type-checking */

    union {
        /* IRON_HIR_EXPR_INT_LIT */
        struct { int64_t value; } int_lit;

        /* IRON_HIR_EXPR_FLOAT_LIT */
        struct { double value; } float_lit;

        /* IRON_HIR_EXPR_STRING_LIT */
        struct { const char *value; } string_lit;

        /* IRON_HIR_EXPR_INTERP_STRING */
        struct {
            IronHIR_Expr **parts;      /* stb_ds array */
            int            part_count;
        } interp_string;

        /* IRON_HIR_EXPR_BOOL_LIT */
        struct { bool value; } bool_lit;

        /* IRON_HIR_EXPR_NULL_LIT: no payload */
        struct { int _pad; } null_lit;

        /* IRON_HIR_EXPR_IDENT */
        struct {
            IronHIR_VarId var_id;
            const char   *name;
        } ident;

        /* IRON_HIR_EXPR_BINOP */
        struct {
            IronHIR_BinOp  op;
            IronHIR_Expr  *left;
            IronHIR_Expr  *right;
        } binop;

        /* IRON_HIR_EXPR_UNOP */
        struct {
            IronHIR_UnOp  op;
            IronHIR_Expr *operand;
        } unop;

        /* IRON_HIR_EXPR_CALL */
        struct {
            IronHIR_Expr  *callee;
            IronHIR_Expr **args;       /* stb_ds array */
            int            arg_count;
        } call;

        /* IRON_HIR_EXPR_METHOD_CALL */
        struct {
            IronHIR_Expr  *object;
            const char    *method;
            IronHIR_Expr **args;       /* stb_ds array */
            int            arg_count;
        } method_call;

        /* IRON_HIR_EXPR_FIELD_ACCESS */
        struct {
            IronHIR_Expr *object;
            const char   *field;
        } field_access;

        /* IRON_HIR_EXPR_INDEX */
        struct {
            IronHIR_Expr *array;
            IronHIR_Expr *index;
        } index;

        /* IRON_HIR_EXPR_SLICE */
        struct {
            IronHIR_Expr *array;
            IronHIR_Expr *start;
            IronHIR_Expr *end;
        } slice;

        /* IRON_HIR_EXPR_CLOSURE */
        struct {
            IronHIR_Param    *params;          /* stb_ds array */
            int               param_count;
            Iron_Type        *return_type;
            IronHIR_Block    *body;
            const char       *lifted_name;     /* assigned top-level name e.g. "__lambda_0" */
            Iron_CaptureEntry *captures;       /* from AST capture analysis */
            int               capture_count;
            IronHIR_VarId    *capture_var_ids; /* VarIds of captured vars in enclosing scope */
        } closure;

        /* IRON_HIR_EXPR_HEAP */
        struct {
            IronHIR_Expr *inner;
            bool          auto_free;
            bool          escapes;
        } heap;

        /* IRON_HIR_EXPR_RC */
        struct {
            IronHIR_Expr *inner;
        } rc;

        /* IRON_HIR_EXPR_CONSTRUCT */
        struct {
            Iron_Type     *type;
            const char   **field_names;  /* stb_ds array */
            IronHIR_Expr **field_values; /* stb_ds array, parallel */
            int            field_count;
        } construct;

        /* IRON_HIR_EXPR_ARRAY_LIT */
        struct {
            Iron_Type     *elem_type;
            IronHIR_Expr **elements;    /* stb_ds array */
            int            element_count;
        } array_lit;

        /* IRON_HIR_EXPR_AWAIT */
        struct {
            IronHIR_Expr *handle;
        } await_expr;

        /* IRON_HIR_EXPR_CAST */
        struct {
            IronHIR_Expr *value;
            Iron_Type    *target_type;
        } cast;

        /* IRON_HIR_EXPR_IS_NULL, IRON_HIR_EXPR_IS_NOT_NULL */
        struct {
            IronHIR_Expr *value;
        } null_check;

        /* IRON_HIR_EXPR_FUNC_REF */
        struct {
            const char *func_name;
        } func_ref;

        /* IRON_HIR_EXPR_PARALLEL_FOR */
        struct {
            IronHIR_VarId  var_id;
            IronHIR_Expr  *range;
            IronHIR_Block *body;
            const char    *lifted_name; /* assigned top-level name e.g. "__pfor_0" */
        } parallel_for;

        /* IRON_HIR_EXPR_COMPTIME */
        struct {
            IronHIR_Expr *inner;
        } comptime;

        /* IRON_HIR_EXPR_IS */
        struct {
            IronHIR_Expr *value;
            Iron_Type    *check_type;
        } is_check;
    };
};

/* ── Function ────────────────────────────────────────────────────────────── */

struct IronHIR_Func {
    const char        *name;
    Iron_Type         *return_type;
    IronHIR_Param     *params;         /* stb_ds array */
    int                param_count;
    IronHIR_Block     *body;
    bool               is_extern;
    const char        *extern_c_name;
    Iron_CaptureEntry *captures;       /* from capture analysis; NULL for non-capturing */
    int                capture_count;
};

/* ── Module ──────────────────────────────────────────────────────────────── */

struct IronHIR_Module {
    const char      *name;
    IronHIR_Func   **funcs;       /* stb_ds array */
    int              func_count;

    /* Named variable table: index == var_id (entry 0 is sentinel) */
    IronHIR_VarInfo *name_table;  /* stb_ds array */
    IronHIR_VarId    next_var_id; /* starts at 1 */

    Iron_Arena      *arena;       /* owning arena */
};

/* ── Constructor declarations ────────────────────────────────────────────── */

/* Module */
IronHIR_Module *iron_hir_module_create(const char *name);
void            iron_hir_module_destroy(IronHIR_Module *mod);

/* Variable allocation */
IronHIR_VarId   iron_hir_alloc_var(IronHIR_Module *mod, const char *name,
                                    Iron_Type *type, bool is_mutable);
const char     *iron_hir_var_name(IronHIR_Module *mod, IronHIR_VarId id);

/* Function */
IronHIR_Func   *iron_hir_func_create(IronHIR_Module *mod, const char *name,
                                      IronHIR_Param *params, int param_count,
                                      Iron_Type *return_type);
void            iron_hir_module_add_func(IronHIR_Module *mod, IronHIR_Func *func);

/* Block */
IronHIR_Block  *iron_hir_block_create(IronHIR_Module *mod);
void            iron_hir_block_add_stmt(IronHIR_Block *block, IronHIR_Stmt *stmt);

/* Statement constructors (13) */
IronHIR_Stmt *iron_hir_stmt_let(IronHIR_Module *mod, IronHIR_VarId var_id,
                                 Iron_Type *type, IronHIR_Expr *init,
                                 bool is_mutable, Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_assign(IronHIR_Module *mod, IronHIR_Expr *target,
                                    IronHIR_Expr *value, Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_if(IronHIR_Module *mod, IronHIR_Expr *condition,
                                IronHIR_Block *then_body, IronHIR_Block *else_body,
                                Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_while(IronHIR_Module *mod, IronHIR_Expr *condition,
                                   IronHIR_Block *body, Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_for(IronHIR_Module *mod, IronHIR_VarId var_id,
                                 IronHIR_Expr *iterable, IronHIR_Block *body,
                                 Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_match(IronHIR_Module *mod, IronHIR_Expr *scrutinee,
                                   IronHIR_MatchArm *arms, int arm_count,
                                   Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_return(IronHIR_Module *mod, IronHIR_Expr *value,
                                    Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_defer(IronHIR_Module *mod, IronHIR_Block *body,
                                   Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_block(IronHIR_Module *mod, IronHIR_Block *block,
                                   Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_expr(IronHIR_Module *mod, IronHIR_Expr *expr,
                                  Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_free(IronHIR_Module *mod, IronHIR_Expr *value,
                                  Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_spawn(IronHIR_Module *mod, const char *handle_name,
                                   IronHIR_Block *body, const char *lifted_name,
                                   Iron_Span span);
IronHIR_Stmt *iron_hir_stmt_leak(IronHIR_Module *mod, IronHIR_Expr *value,
                                  Iron_Span span);

/* Expression constructors (28) */
IronHIR_Expr *iron_hir_expr_int_lit(IronHIR_Module *mod, int64_t value,
                                     Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_float_lit(IronHIR_Module *mod, double value,
                                       Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_string_lit(IronHIR_Module *mod, const char *value,
                                        Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_interp_string(IronHIR_Module *mod,
                                           IronHIR_Expr **parts, int part_count,
                                           Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_bool_lit(IronHIR_Module *mod, bool value,
                                      Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_null_lit(IronHIR_Module *mod, Iron_Type *type,
                                      Iron_Span span);
IronHIR_Expr *iron_hir_expr_ident(IronHIR_Module *mod, IronHIR_VarId var_id,
                                   const char *name, Iron_Type *type,
                                   Iron_Span span);
IronHIR_Expr *iron_hir_expr_binop(IronHIR_Module *mod, IronHIR_BinOp op,
                                   IronHIR_Expr *left, IronHIR_Expr *right,
                                   Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_unop(IronHIR_Module *mod, IronHIR_UnOp op,
                                  IronHIR_Expr *operand,
                                  Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_call(IronHIR_Module *mod, IronHIR_Expr *callee,
                                  IronHIR_Expr **args, int arg_count,
                                  Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_method_call(IronHIR_Module *mod, IronHIR_Expr *object,
                                         const char *method,
                                         IronHIR_Expr **args, int arg_count,
                                         Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_field_access(IronHIR_Module *mod, IronHIR_Expr *object,
                                          const char *field,
                                          Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_index(IronHIR_Module *mod, IronHIR_Expr *array,
                                   IronHIR_Expr *index,
                                   Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_slice(IronHIR_Module *mod, IronHIR_Expr *array,
                                   IronHIR_Expr *start, IronHIR_Expr *end,
                                   Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_closure(IronHIR_Module *mod,
                                     IronHIR_Param *params, int param_count,
                                     Iron_Type *return_type, IronHIR_Block *body,
                                     Iron_Type *type, const char *lifted_name,
                                     Iron_CaptureEntry *captures, int capture_count,
                                     Iron_Span span);
IronHIR_Expr *iron_hir_expr_heap(IronHIR_Module *mod, IronHIR_Expr *inner,
                                  bool auto_free, bool escapes,
                                  Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_rc(IronHIR_Module *mod, IronHIR_Expr *inner,
                                Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_construct(IronHIR_Module *mod, Iron_Type *type,
                                       const char **field_names,
                                       IronHIR_Expr **field_values, int field_count,
                                       Iron_Span span);
IronHIR_Expr *iron_hir_expr_array_lit(IronHIR_Module *mod, Iron_Type *elem_type,
                                       IronHIR_Expr **elements, int element_count,
                                       Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_await(IronHIR_Module *mod, IronHIR_Expr *handle,
                                   Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_cast(IronHIR_Module *mod, IronHIR_Expr *value,
                                  Iron_Type *target_type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_is_null(IronHIR_Module *mod, IronHIR_Expr *value,
                                     Iron_Span span);
IronHIR_Expr *iron_hir_expr_is_not_null(IronHIR_Module *mod, IronHIR_Expr *value,
                                         Iron_Span span);
IronHIR_Expr *iron_hir_expr_func_ref(IronHIR_Module *mod, const char *func_name,
                                      Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_parallel_for(IronHIR_Module *mod,
                                          IronHIR_VarId var_id,
                                          IronHIR_Expr *range,
                                          IronHIR_Block *body,
                                          Iron_Type *type, const char *lifted_name,
                                          Iron_Span span);
IronHIR_Expr *iron_hir_expr_comptime(IronHIR_Module *mod, IronHIR_Expr *inner,
                                      Iron_Type *type, Iron_Span span);
IronHIR_Expr *iron_hir_expr_is(IronHIR_Module *mod, IronHIR_Expr *value,
                                Iron_Type *check_type, Iron_Span span);

/* ---- Printer ---- */
char *iron_hir_print(const IronHIR_Module *module);

/* ---- Verifier ---- */
bool iron_hir_verify(const IronHIR_Module *module, Iron_DiagList *diags,
                     Iron_Arena *arena);

#endif /* IRON_HIR_H */
