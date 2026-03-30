#include "hir/hir.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdio.h>

/* ── Scope tracking ───────────────────────────────────────────────────────── */

/* A scope frame maps VarId (int key) -> 1 (declared).
 * We use stb_ds hash maps keyed by int.
 */
typedef struct {
    int key;
    int value;
} ScopeEntry;

/* Scope stack: dynamic array of hash-map pointers.
 * Each element is a ScopeEntry* (stb_ds hash map).
 */
typedef ScopeEntry **ScopeStack;

/* Forward declarations */
static void verify_block(const IronHIR_Block *block, const IronHIR_Module *mod,
                          ScopeStack *stack, Iron_DiagList *diags, Iron_Arena *arena);
static void verify_stmt(const IronHIR_Stmt *stmt, const IronHIR_Module *mod,
                         ScopeStack *stack, Iron_DiagList *diags, Iron_Arena *arena);
static void verify_expr(const IronHIR_Expr *expr, const IronHIR_Module *mod,
                         ScopeStack *stack, Iron_DiagList *diags, Iron_Arena *arena);

/* ── Scope helpers ────────────────────────────────────────────────────────── */

static void push_scope(ScopeStack *stack) {
    ScopeEntry *frame = NULL;  /* empty stb_ds hash map */
    arrput(*stack, frame);
}

static void pop_scope(ScopeStack *stack) {
    int top = (int)arrlen(*stack) - 1;
    if (top < 0) return;
    hmfree((*stack)[top]);
    arrdel(*stack, top);
}

/* Declare a variable in the top scope. Emits IRON_ERR_HIR_DUPLICATE_BINDING if
 * the same var_id is already declared in the current (top) scope. */
static void declare_var(ScopeStack *stack, IronHIR_VarId var_id,
                         const IronHIR_Module *mod, Iron_Span span,
                         Iron_DiagList *diags, Iron_Arena *arena) {
    if (var_id == IRON_HIR_VAR_INVALID) return;
    int top = (int)arrlen(*stack) - 1;
    if (top < 0) return;

    ScopeEntry *frame = (*stack)[top];
    /* Check for duplicate in current scope */
    int existing = hmgeti(frame, (int)var_id);
    if (existing >= 0) {
        /* Duplicate binding in the same scope */
        const char *name = iron_hir_var_name((IronHIR_Module *)mod, var_id);
        char msg[256];
        snprintf(msg, sizeof(msg), "duplicate binding '%s' in the same scope", name);
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_HIR_DUPLICATE_BINDING,
                       span, msg,
                       "rename this binding or remove the previous declaration");
        return;
    }
    hmput(frame, (int)var_id, 1);
    (*stack)[top] = frame;
}

/* Look up a variable in the scope stack (top to bottom). Returns true if found. */
static bool lookup_var(ScopeStack *stack, IronHIR_VarId var_id) {
    if (var_id == IRON_HIR_VAR_INVALID) return false;
    int n = (int)arrlen(*stack);
    for (int i = n - 1; i >= 0; i--) {
        ScopeEntry *frame = (*stack)[i];
        int idx = hmgeti(frame, (int)var_id);
        if (idx >= 0) return true;
    }
    return false;
}

/* ── Verifier helpers ─────────────────────────────────────────────────────── */

static void verify_func(const IronHIR_Func *func, const IronHIR_Module *mod,
                         Iron_DiagList *diags, Iron_Arena *arena) {
    /* Null checks */
    if (!func->name) {
        Iron_Span span = iron_span_make("<hir>", 0, 0, 0, 0);
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_HIR_NULL_POINTER, span,
                       "function has NULL name",
                       "provide a function name");
    }
    if (!func->body) {
        Iron_Span span = iron_span_make(func->name ? func->name : "<hir>", 0, 0, 0, 0);
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_HIR_NULL_POINTER, span,
                       "function body is NULL",
                       "provide a function body block");
        return;
    }

    /* Build scope stack */
    ScopeStack stack = NULL;
    push_scope(&stack);

    /* Declare all parameters in function scope */
    for (int i = 0; i < func->param_count; i++) {
        const IronHIR_Param *p = &func->params[i];
        Iron_Span span = iron_span_make(func->name ? func->name : "<hir>", 0, 0, 0, 0);
        declare_var(&stack, p->var_id, mod, span, diags, arena);
    }

    verify_block(func->body, mod, &stack, diags, arena);

    pop_scope(&stack);
    arrfree(stack);
}

static void verify_block(const IronHIR_Block *block, const IronHIR_Module *mod,
                          ScopeStack *stack, Iron_DiagList *diags, Iron_Arena *arena) {
    if (!block) return;
    push_scope(stack);
    for (int i = 0; i < block->stmt_count; i++) {
        verify_stmt(block->stmts[i], mod, stack, diags, arena);
    }
    pop_scope(stack);
}

static void verify_stmt(const IronHIR_Stmt *stmt, const IronHIR_Module *mod,
                         ScopeStack *stack, Iron_DiagList *diags, Iron_Arena *arena) {
    if (!stmt) {
        Iron_Span span = iron_span_make("<hir>", 0, 0, 0, 0);
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_HIR_NULL_POINTER, span,
                       "NULL statement encountered",
                       "ensure all statement pointers are valid");
        return;
    }

    switch (stmt->kind) {

    case IRON_HIR_STMT_LET:
        /* Verify init expr first, then declare var (so init cannot reference self) */
        if (stmt->let.init) {
            verify_expr(stmt->let.init, mod, stack, diags, arena);
        }
        declare_var(stack, stmt->let.var_id, mod, stmt->span, diags, arena);
        break;

    case IRON_HIR_STMT_ASSIGN:
        if (stmt->assign.target) {
            verify_expr(stmt->assign.target, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, stmt->span,
                           "assign statement has NULL target",
                           "provide a valid lvalue target");
        }
        if (stmt->assign.value) {
            verify_expr(stmt->assign.value, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, stmt->span,
                           "assign statement has NULL value",
                           "provide a valid expression value");
        }
        break;

    case IRON_HIR_STMT_IF:
        if (stmt->if_else.condition) {
            verify_expr(stmt->if_else.condition, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, stmt->span,
                           "if statement has NULL condition",
                           "provide a condition expression");
        }
        if (stmt->if_else.then_body) {
            verify_block(stmt->if_else.then_body, mod, stack, diags, arena);
        }
        if (stmt->if_else.else_body) {
            verify_block(stmt->if_else.else_body, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_WHILE:
        if (stmt->while_loop.condition) {
            verify_expr(stmt->while_loop.condition, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, stmt->span,
                           "while statement has NULL condition",
                           "provide a loop condition");
        }
        if (stmt->while_loop.body) {
            verify_block(stmt->while_loop.body, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_FOR:
        /* Introduce a scope for the loop variable */
        push_scope(stack);
        /* Declare loop variable before verifying iterable/body */
        declare_var(stack, stmt->for_loop.var_id, mod, stmt->span, diags, arena);
        if (stmt->for_loop.iterable) {
            verify_expr(stmt->for_loop.iterable, mod, stack, diags, arena);
        }
        if (stmt->for_loop.body) {
            verify_block(stmt->for_loop.body, mod, stack, diags, arena);
        }
        pop_scope(stack);
        break;

    case IRON_HIR_STMT_MATCH:
        if (stmt->match_stmt.scrutinee) {
            verify_expr(stmt->match_stmt.scrutinee, mod, stack, diags, arena);
        }
        for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
            const IronHIR_MatchArm *arm = &stmt->match_stmt.arms[i];
            push_scope(stack);
            if (arm->pattern) {
                verify_expr(arm->pattern, mod, stack, diags, arena);
            }
            if (arm->guard) {
                verify_expr(arm->guard, mod, stack, diags, arena);
            }
            if (arm->body) {
                verify_block(arm->body, mod, stack, diags, arena);
            }
            pop_scope(stack);
        }
        break;

    case IRON_HIR_STMT_RETURN:
        if (stmt->return_stmt.value) {
            verify_expr(stmt->return_stmt.value, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_DEFER:
        if (stmt->defer.body) {
            verify_block(stmt->defer.body, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_BLOCK:
        if (stmt->block.block) {
            verify_block(stmt->block.block, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_EXPR:
        if (stmt->expr_stmt.expr) {
            verify_expr(stmt->expr_stmt.expr, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_FREE:
        if (stmt->free_stmt.value) {
            verify_expr(stmt->free_stmt.value, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, stmt->span,
                           "free statement has NULL value",
                           "provide the pointer to free");
        }
        break;

    case IRON_HIR_STMT_SPAWN:
        if (stmt->spawn.body) {
            verify_block(stmt->spawn.body, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_STMT_LEAK:
        if (stmt->leak.value) {
            verify_expr(stmt->leak.value, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, stmt->span,
                           "leak statement has NULL value",
                           "provide the pointer to leak");
        }
        break;

    default:
        break;
    }
}

static void verify_expr(const IronHIR_Expr *expr, const IronHIR_Module *mod,
                         ScopeStack *stack, Iron_DiagList *diags, Iron_Arena *arena) {
    if (!expr) {
        Iron_Span span = iron_span_make("<hir>", 0, 0, 0, 0);
        iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                       IRON_ERR_HIR_NULL_POINTER, span,
                       "NULL expression encountered",
                       "ensure all expression pointers are valid");
        return;
    }

    switch (expr->kind) {

    /* Literals: no sub-expressions to verify */
    case IRON_HIR_EXPR_INT_LIT:
    case IRON_HIR_EXPR_FLOAT_LIT:
    case IRON_HIR_EXPR_STRING_LIT:
    case IRON_HIR_EXPR_BOOL_LIT:
    case IRON_HIR_EXPR_NULL_LIT:
    case IRON_HIR_EXPR_FUNC_REF:
        break;

    case IRON_HIR_EXPR_INTERP_STRING:
        for (int i = 0; i < expr->interp_string.part_count; i++) {
            verify_expr(expr->interp_string.parts[i], mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_IDENT: {
        IronHIR_VarId id = expr->ident.var_id;
        if (id == IRON_HIR_VAR_INVALID) {
            /* Anonymous ident — allow */
            break;
        }
        if (!lookup_var(stack, id)) {
            const char *name = expr->ident.name
                ? expr->ident.name
                : iron_hir_var_name((IronHIR_Module *)mod, id);
            char msg[256];
            snprintf(msg, sizeof(msg), "use of undeclared variable '%s'", name);
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_USE_BEFORE_DEF,
                           expr->span, msg,
                           "declare the variable before using it");
        }
        break;
    }

    case IRON_HIR_EXPR_BINOP:
        if (!expr->binop.left) {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "binop has NULL left operand",
                           "provide a left-hand side expression");
        } else {
            verify_expr(expr->binop.left, mod, stack, diags, arena);
        }
        if (!expr->binop.right) {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "binop has NULL right operand",
                           "provide a right-hand side expression");
        } else {
            verify_expr(expr->binop.right, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_UNOP:
        if (!expr->unop.operand) {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "unop has NULL operand",
                           "provide an operand expression");
        } else {
            verify_expr(expr->unop.operand, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_CALL:
        if (expr->call.callee) {
            verify_expr(expr->call.callee, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "call expression has NULL callee",
                           "provide a callee expression");
        }
        for (int i = 0; i < expr->call.arg_count; i++) {
            verify_expr(expr->call.args[i], mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_METHOD_CALL:
        if (expr->method_call.object) {
            verify_expr(expr->method_call.object, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "method call has NULL object",
                           "provide a receiver object");
        }
        for (int i = 0; i < expr->method_call.arg_count; i++) {
            verify_expr(expr->method_call.args[i], mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_FIELD_ACCESS:
        if (expr->field_access.object) {
            verify_expr(expr->field_access.object, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "field access has NULL object",
                           "provide a receiver object");
        }
        break;

    case IRON_HIR_EXPR_INDEX:
        if (expr->index.array) {
            verify_expr(expr->index.array, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "index expression has NULL array",
                           "provide an array expression");
        }
        if (expr->index.index) {
            verify_expr(expr->index.index, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "index expression has NULL index",
                           "provide an index expression");
        }
        break;

    case IRON_HIR_EXPR_SLICE:
        if (expr->slice.array) {
            verify_expr(expr->slice.array, mod, stack, diags, arena);
        }
        if (expr->slice.start) {
            verify_expr(expr->slice.start, mod, stack, diags, arena);
        }
        if (expr->slice.end) {
            verify_expr(expr->slice.end, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_CLOSURE:
        /* Push a new scope for closure params */
        push_scope(stack);
        for (int i = 0; i < expr->closure.param_count; i++) {
            const IronHIR_Param *p = &expr->closure.params[i];
            declare_var(stack, p->var_id, mod, expr->span, diags, arena);
        }
        if (expr->closure.body) {
            verify_block(expr->closure.body, mod, stack, diags, arena);
        }
        pop_scope(stack);
        break;

    case IRON_HIR_EXPR_PARALLEL_FOR:
        push_scope(stack);
        declare_var(stack, expr->parallel_for.var_id, mod, expr->span, diags, arena);
        if (expr->parallel_for.range) {
            verify_expr(expr->parallel_for.range, mod, stack, diags, arena);
        }
        if (expr->parallel_for.body) {
            verify_block(expr->parallel_for.body, mod, stack, diags, arena);
        }
        pop_scope(stack);
        break;

    case IRON_HIR_EXPR_COMPTIME:
        if (expr->comptime.inner) {
            verify_expr(expr->comptime.inner, mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_HEAP:
        if (expr->heap.inner) {
            verify_expr(expr->heap.inner, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "heap expression has NULL inner value",
                           "provide an inner expression");
        }
        break;

    case IRON_HIR_EXPR_RC:
        if (expr->rc.inner) {
            verify_expr(expr->rc.inner, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "rc expression has NULL inner value",
                           "provide an inner expression");
        }
        break;

    case IRON_HIR_EXPR_CONSTRUCT:
        for (int i = 0; i < expr->construct.field_count; i++) {
            if (expr->construct.field_values) {
                verify_expr(expr->construct.field_values[i], mod, stack, diags, arena);
            }
        }
        break;

    case IRON_HIR_EXPR_ARRAY_LIT:
        for (int i = 0; i < expr->array_lit.element_count; i++) {
            verify_expr(expr->array_lit.elements[i], mod, stack, diags, arena);
        }
        break;

    case IRON_HIR_EXPR_AWAIT:
        if (expr->await_expr.handle) {
            verify_expr(expr->await_expr.handle, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "await expression has NULL handle",
                           "provide a handle expression");
        }
        break;

    case IRON_HIR_EXPR_CAST:
        if (expr->cast.value) {
            verify_expr(expr->cast.value, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "cast expression has NULL value",
                           "provide a value to cast");
        }
        break;

    case IRON_HIR_EXPR_IS_NULL:
    case IRON_HIR_EXPR_IS_NOT_NULL:
        if (expr->null_check.value) {
            verify_expr(expr->null_check.value, mod, stack, diags, arena);
        } else {
            iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                           IRON_ERR_HIR_STRUCTURAL, expr->span,
                           "null check has NULL value",
                           "provide a value to check");
        }
        break;

    case IRON_HIR_EXPR_IS:
        if (expr->is_check.value) {
            verify_expr(expr->is_check.value, mod, stack, diags, arena);
        }
        break;

    default:
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool iron_hir_verify(const IronHIR_Module *module, Iron_DiagList *diags,
                     Iron_Arena *arena) {
    if (!module || !diags || !arena) return false;

    int errors_before = diags->error_count;

    for (int i = 0; i < module->func_count; i++) {
        verify_func(module->funcs[i], module, diags, arena);
    }

    return diags->error_count == errors_before;
}
