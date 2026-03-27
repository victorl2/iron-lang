#ifndef IRON_IR_LOWER_INTERNAL_H
#define IRON_IR_LOWER_INTERNAL_H

#include "ir/ir.h"
#include "ir/lower.h"
#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* ── Lift descriptor for post-pass lambda/spawn/pfor lifting ─────────── */

typedef enum {
    LIFT_LAMBDA,
    LIFT_SPAWN,
    LIFT_PARALLEL_FOR
} LiftKind;

typedef struct {
    LiftKind      kind;
    Iron_Node    *ast_node;        /* the lambda/spawn/pfor AST node */
    const char   *lifted_name;     /* assigned name (__lambda_0, etc.) */
    const char   *enclosing_func;  /* name of the function containing this */
} LiftPending;

/* ── Lowering context ────────────────────────────────────────────────────── */

typedef struct {
    /* Inputs (owned by caller) */
    Iron_Program    *program;
    Iron_Scope      *global_scope;
    Iron_Arena      *ir_arena;
    Iron_DiagList   *diags;

    /* Output being built */
    IronIR_Module   *module;

    /* Current function being lowered */
    IronIR_Func     *current_func;
    IronIR_Block    *current_block;

    /* val binding table: name -> ValueId (immutable, no alloca) */
    struct { char *key; IronIR_ValueId value; } *val_binding_map;

    /* var alloca table: name -> ValueId of alloca instruction */
    struct { char *key; IronIR_ValueId value; } *var_alloca_map;

    /* Function parameter ValueId table: name -> ValueId */
    struct { char *key; IronIR_ValueId value; } *param_map;

    /* Defer stack: stb_ds array of (stb_ds array of Iron_Node*) */
    Iron_Node  ***defer_stacks;
    int           defer_depth;
    int           function_scope_depth;

    /* Pending lifts: collected during Pass 2, processed in lifting pass */
    LiftPending   *pending_lifts;  /* stb_ds array */

    /* Current enclosing function name (for lifted function naming) */
    const char    *current_func_name;

    /* Loop context for break/continue */
    IronIR_Block  *loop_exit_block;
    IronIR_Block  *loop_continue_block;
    int            loop_scope_depth;
} IronIR_LowerCtx;

/* ── Shared helper declarations (defined in lower.c) ─────────────────── */

/* Lower a single expression node, returning its ValueId */
IronIR_ValueId lower_expr(IronIR_LowerCtx *ctx, Iron_Node *node);

/* Lower a single statement node */
void lower_stmt(IronIR_LowerCtx *ctx, Iron_Node *node);

/* Create a new basic block and append to current function */
IronIR_Block *new_block(IronIR_LowerCtx *ctx, const char *label);

/* Switch current_block to the given block */
void switch_block(IronIR_LowerCtx *ctx, IronIR_Block *block);

/* Emit deferred calls from current scope down to target_depth */
void emit_defers_ir(IronIR_LowerCtx *ctx, int target_depth);

/* Emit a poison instruction as error placeholder, returns its ValueId */
IronIR_ValueId emit_poison(IronIR_LowerCtx *ctx, Iron_Type *type, Iron_Span span);

/* Push/pop defer scope */
void push_defer_scope(IronIR_LowerCtx *ctx);
void pop_defer_scope(IronIR_LowerCtx *ctx);

/* Lower a block of statements (pushes defer scope, lowers stmts, drains defers, pops scope) */
void lower_block(IronIR_LowerCtx *ctx, Iron_Block *block);

/* ── Pass functions (defined in lower_types.c, lower_stmts.c) ────────── */

/* Pass 1: Register all module-level declarations */
void lower_module_decls(IronIR_LowerCtx *ctx);

/* Post-pass: Lift pending lambdas/spawn/parallel-for to top-level functions */
void lower_lift_pending(IronIR_LowerCtx *ctx);

#endif /* IRON_IR_LOWER_INTERNAL_H */
