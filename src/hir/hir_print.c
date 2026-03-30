#include "hir/hir.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "analyzer/types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Forward declarations ────────────────────────────────────────────────── */

static void print_block(Iron_StrBuf *sb, const IronHIR_Block *block,
                         const IronHIR_Module *mod, int depth, Iron_Arena *tmp);
static void print_stmt(Iron_StrBuf *sb, const IronHIR_Stmt *stmt,
                        const IronHIR_Module *mod, int depth, Iron_Arena *tmp);
static void print_expr(Iron_StrBuf *sb, const IronHIR_Expr *expr,
                        const IronHIR_Module *mod, int depth, Iron_Arena *tmp);

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Emit depth*2 spaces for indentation. */
static void do_indent(Iron_StrBuf *sb, int depth) {
    for (int i = 0; i < depth * 2; i++) {
        iron_strbuf_append_char(sb, ' ');
    }
}

/* Append a type string, using "?" for NULL types. */
static void append_type(Iron_StrBuf *sb, const Iron_Type *type, Iron_Arena *tmp) {
    if (!type) {
        iron_strbuf_appendf(sb, "?");
        return;
    }
    const char *ts = iron_type_to_string(type, tmp);
    iron_strbuf_appendf(sb, "%s", ts);
}

/* Return a string label for a BinOp. */
static const char *binop_str(IronHIR_BinOp op) {
    switch (op) {
    case IRON_HIR_BINOP_ADD: return "+";
    case IRON_HIR_BINOP_SUB: return "-";
    case IRON_HIR_BINOP_MUL: return "*";
    case IRON_HIR_BINOP_DIV: return "/";
    case IRON_HIR_BINOP_MOD: return "%";
    case IRON_HIR_BINOP_EQ:  return "==";
    case IRON_HIR_BINOP_NEQ: return "!=";
    case IRON_HIR_BINOP_LT:  return "<";
    case IRON_HIR_BINOP_LTE: return "<=";
    case IRON_HIR_BINOP_GT:  return ">";
    case IRON_HIR_BINOP_GTE: return ">=";
    case IRON_HIR_BINOP_AND: return "&&";
    case IRON_HIR_BINOP_OR:  return "||";
    default:                 return "?";
    }
}

/* Return a string label for a UnOp. */
static const char *unop_str(IronHIR_UnOp op) {
    switch (op) {
    case IRON_HIR_UNOP_NEG: return "-";
    case IRON_HIR_UNOP_NOT: return "!";
    default:                return "?";
    }
}

/* Resolve a VarId to a name (safely). */
static const char *var_name(const IronHIR_Module *mod, IronHIR_VarId id) {
    if (!mod || id == IRON_HIR_VAR_INVALID) return "<invalid>";
    if ((ptrdiff_t)id >= arrlen(mod->name_table)) return "<unknown>";
    return mod->name_table[id].name;
}

/* ── Printer helpers ──────────────────────────────────────────────────────── */

static void print_func(Iron_StrBuf *sb, const IronHIR_Func *func,
                        const IronHIR_Module *mod, int depth, Iron_Arena *tmp) {
    do_indent(sb, depth);
    iron_strbuf_appendf(sb, "FuncDecl(%s) -> ", func->name ? func->name : "<null>");
    append_type(sb, func->return_type, tmp);
    iron_strbuf_appendf(sb, "\n");

    /* Print params */
    for (int i = 0; i < func->param_count; i++) {
        const IronHIR_Param *p = &func->params[i];
        do_indent(sb, depth + 1);
        iron_strbuf_appendf(sb, "Param(%s: ", p->name ? p->name : "<null>");
        append_type(sb, p->type, tmp);
        iron_strbuf_appendf(sb, ")\n");
    }

    /* Print body block */
    if (func->body) {
        print_block(sb, func->body, mod, depth + 1, tmp);
    }
}

static void print_block(Iron_StrBuf *sb, const IronHIR_Block *block,
                         const IronHIR_Module *mod, int depth, Iron_Arena *tmp) {
    if (!block) return;
    do_indent(sb, depth);
    iron_strbuf_appendf(sb, "Block\n");
    for (int i = 0; i < block->stmt_count; i++) {
        print_stmt(sb, block->stmts[i], mod, depth + 1, tmp);
    }
}

static void print_stmt(Iron_StrBuf *sb, const IronHIR_Stmt *stmt,
                        const IronHIR_Module *mod, int depth, Iron_Arena *tmp) {
    if (!stmt) return;
    switch (stmt->kind) {

    case IRON_HIR_STMT_LET: {
        const char *kw = stmt->let.is_mutable ? "VarStmt" : "LetStmt";
        const char *name = var_name(mod, stmt->let.var_id);
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "%s(%s: ", kw, name);
        append_type(sb, stmt->let.type, tmp);
        iron_strbuf_appendf(sb, ")\n");
        if (stmt->let.init) {
            print_expr(sb, stmt->let.init, mod, depth + 1, tmp);
        }
        break;
    }

    case IRON_HIR_STMT_ASSIGN:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "AssignStmt\n");
        if (stmt->assign.target) {
            print_expr(sb, stmt->assign.target, mod, depth + 1, tmp);
        }
        if (stmt->assign.value) {
            print_expr(sb, stmt->assign.value, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_IF:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "IfStmt\n");
        if (stmt->if_else.condition) {
            do_indent(sb, depth + 1);
            iron_strbuf_appendf(sb, "Cond\n");
            print_expr(sb, stmt->if_else.condition, mod, depth + 2, tmp);
        }
        if (stmt->if_else.then_body) {
            print_block(sb, stmt->if_else.then_body, mod, depth + 1, tmp);
        }
        if (stmt->if_else.else_body) {
            do_indent(sb, depth + 1);
            iron_strbuf_appendf(sb, "Else\n");
            print_block(sb, stmt->if_else.else_body, mod, depth + 2, tmp);
        }
        break;

    case IRON_HIR_STMT_WHILE:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "WhileStmt\n");
        if (stmt->while_loop.condition) {
            print_expr(sb, stmt->while_loop.condition, mod, depth + 1, tmp);
        }
        if (stmt->while_loop.body) {
            print_block(sb, stmt->while_loop.body, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_FOR: {
        const char *name = var_name(mod, stmt->for_loop.var_id);
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "ForStmt(%s)\n", name);
        if (stmt->for_loop.iterable) {
            print_expr(sb, stmt->for_loop.iterable, mod, depth + 1, tmp);
        }
        if (stmt->for_loop.body) {
            print_block(sb, stmt->for_loop.body, mod, depth + 1, tmp);
        }
        break;
    }

    case IRON_HIR_STMT_MATCH:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "MatchStmt\n");
        if (stmt->match_stmt.scrutinee) {
            print_expr(sb, stmt->match_stmt.scrutinee, mod, depth + 1, tmp);
        }
        for (int i = 0; i < stmt->match_stmt.arm_count; i++) {
            const IronHIR_MatchArm *arm = &stmt->match_stmt.arms[i];
            do_indent(sb, depth + 1);
            iron_strbuf_appendf(sb, "MatchArm\n");
            if (arm->pattern) {
                print_expr(sb, arm->pattern, mod, depth + 2, tmp);
            }
            if (arm->guard) {
                do_indent(sb, depth + 2);
                iron_strbuf_appendf(sb, "Guard\n");
                print_expr(sb, arm->guard, mod, depth + 3, tmp);
            }
            if (arm->body) {
                print_block(sb, arm->body, mod, depth + 2, tmp);
            }
        }
        break;

    case IRON_HIR_STMT_RETURN:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "ReturnStmt\n");
        if (stmt->return_stmt.value) {
            print_expr(sb, stmt->return_stmt.value, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_DEFER:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "DeferStmt\n");
        if (stmt->defer.body) {
            print_block(sb, stmt->defer.body, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_BLOCK:
        print_block(sb, stmt->block.block, mod, depth, tmp);
        break;

    case IRON_HIR_STMT_EXPR:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "ExprStmt\n");
        if (stmt->expr_stmt.expr) {
            print_expr(sb, stmt->expr_stmt.expr, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_FREE:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "FreeStmt\n");
        if (stmt->free_stmt.value) {
            print_expr(sb, stmt->free_stmt.value, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_SPAWN:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "SpawnStmt\n");
        if (stmt->spawn.body) {
            print_block(sb, stmt->spawn.body, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_STMT_LEAK:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "LeakStmt\n");
        if (stmt->leak.value) {
            print_expr(sb, stmt->leak.value, mod, depth + 1, tmp);
        }
        break;

    default:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "UnknownStmt\n");
        break;
    }
}

static void print_expr(Iron_StrBuf *sb, const IronHIR_Expr *expr,
                        const IronHIR_Module *mod, int depth, Iron_Arena *tmp) {
    if (!expr) return;
    switch (expr->kind) {

    case IRON_HIR_EXPR_INT_LIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Literal(%lld)\n", (long long)expr->int_lit.value);
        break;

    case IRON_HIR_EXPR_FLOAT_LIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Literal(%g)\n", expr->float_lit.value);
        break;

    case IRON_HIR_EXPR_STRING_LIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Literal(\"%s\")\n",
                            expr->string_lit.value ? expr->string_lit.value : "");
        break;

    case IRON_HIR_EXPR_INTERP_STRING:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "InterpString\n");
        for (int i = 0; i < expr->interp_string.part_count; i++) {
            print_expr(sb, expr->interp_string.parts[i], mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_BOOL_LIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Literal(%s)\n",
                            expr->bool_lit.value ? "true" : "false");
        break;

    case IRON_HIR_EXPR_NULL_LIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Literal(null)\n");
        break;

    case IRON_HIR_EXPR_IDENT: {
        const char *name = expr->ident.name
            ? expr->ident.name
            : var_name(mod, expr->ident.var_id);
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Ident(%s)\n", name);
        break;
    }

    case IRON_HIR_EXPR_BINOP:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "BinOp(%s)\n", binop_str(expr->binop.op));
        print_expr(sb, expr->binop.left,  mod, depth + 1, tmp);
        print_expr(sb, expr->binop.right, mod, depth + 1, tmp);
        break;

    case IRON_HIR_EXPR_UNOP:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "UnOp(%s)\n", unop_str(expr->unop.op));
        print_expr(sb, expr->unop.operand, mod, depth + 1, tmp);
        break;

    case IRON_HIR_EXPR_CALL:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Call\n");
        if (expr->call.callee) {
            print_expr(sb, expr->call.callee, mod, depth + 1, tmp);
        }
        for (int i = 0; i < expr->call.arg_count; i++) {
            print_expr(sb, expr->call.args[i], mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_METHOD_CALL:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "MethodCall(%s)\n",
                            expr->method_call.method ? expr->method_call.method : "?");
        if (expr->method_call.object) {
            print_expr(sb, expr->method_call.object, mod, depth + 1, tmp);
        }
        for (int i = 0; i < expr->method_call.arg_count; i++) {
            print_expr(sb, expr->method_call.args[i], mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_FIELD_ACCESS:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "FieldAccess(%s)\n",
                            expr->field_access.field ? expr->field_access.field : "?");
        if (expr->field_access.object) {
            print_expr(sb, expr->field_access.object, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_INDEX:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Index\n");
        if (expr->index.array) {
            print_expr(sb, expr->index.array, mod, depth + 1, tmp);
        }
        if (expr->index.index) {
            print_expr(sb, expr->index.index, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_SLICE:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Slice\n");
        if (expr->slice.array) {
            print_expr(sb, expr->slice.array, mod, depth + 1, tmp);
        }
        if (expr->slice.start) {
            print_expr(sb, expr->slice.start, mod, depth + 1, tmp);
        }
        if (expr->slice.end) {
            print_expr(sb, expr->slice.end, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_CLOSURE:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Closure\n");
        for (int i = 0; i < expr->closure.param_count; i++) {
            const IronHIR_Param *p = &expr->closure.params[i];
            do_indent(sb, depth + 1);
            iron_strbuf_appendf(sb, "Param(%s: ", p->name ? p->name : "<null>");
            append_type(sb, p->type, tmp);
            iron_strbuf_appendf(sb, ")\n");
        }
        if (expr->closure.body) {
            print_block(sb, expr->closure.body, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_PARALLEL_FOR: {
        const char *name = var_name(mod, expr->parallel_for.var_id);
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "ParallelFor(%s)\n", name);
        if (expr->parallel_for.range) {
            print_expr(sb, expr->parallel_for.range, mod, depth + 1, tmp);
        }
        if (expr->parallel_for.body) {
            print_block(sb, expr->parallel_for.body, mod, depth + 1, tmp);
        }
        break;
    }

    case IRON_HIR_EXPR_COMPTIME:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Comptime\n");
        if (expr->comptime.inner) {
            print_expr(sb, expr->comptime.inner, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_HEAP:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Heap\n");
        if (expr->heap.inner) {
            print_expr(sb, expr->heap.inner, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_RC:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Rc\n");
        if (expr->rc.inner) {
            print_expr(sb, expr->rc.inner, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_CONSTRUCT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Construct\n");
        for (int i = 0; i < expr->construct.field_count; i++) {
            do_indent(sb, depth + 1);
            iron_strbuf_appendf(sb, "Field(%s)\n",
                                expr->construct.field_names
                                    ? expr->construct.field_names[i]
                                    : "?");
            if (expr->construct.field_values) {
                print_expr(sb, expr->construct.field_values[i], mod, depth + 2, tmp);
            }
        }
        break;

    case IRON_HIR_EXPR_ARRAY_LIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "ArrayLit\n");
        for (int i = 0; i < expr->array_lit.element_count; i++) {
            print_expr(sb, expr->array_lit.elements[i], mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_AWAIT:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Await\n");
        if (expr->await_expr.handle) {
            print_expr(sb, expr->await_expr.handle, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_CAST:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Cast(");
        append_type(sb, expr->cast.target_type, tmp);
        iron_strbuf_appendf(sb, ")\n");
        if (expr->cast.value) {
            print_expr(sb, expr->cast.value, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_IS_NULL:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "IsNull\n");
        if (expr->null_check.value) {
            print_expr(sb, expr->null_check.value, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_IS_NOT_NULL:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "IsNotNull\n");
        if (expr->null_check.value) {
            print_expr(sb, expr->null_check.value, mod, depth + 1, tmp);
        }
        break;

    case IRON_HIR_EXPR_FUNC_REF:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "FuncRef(%s)\n",
                            expr->func_ref.func_name ? expr->func_ref.func_name : "?");
        break;

    case IRON_HIR_EXPR_IS:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "Is(");
        append_type(sb, expr->is_check.check_type, tmp);
        iron_strbuf_appendf(sb, ")\n");
        if (expr->is_check.value) {
            print_expr(sb, expr->is_check.value, mod, depth + 1, tmp);
        }
        break;

    default:
        do_indent(sb, depth);
        iron_strbuf_appendf(sb, "UnknownExpr\n");
        break;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

char *iron_hir_print(const IronHIR_Module *module) {
    if (!module) return NULL;

    Iron_StrBuf sb = iron_strbuf_create(4096);

    /* Temporary arena for iron_type_to_string calls */
    Iron_Arena tmp = iron_arena_create(4096);
    iron_types_init(&tmp);

    for (int i = 0; i < module->func_count; i++) {
        print_func(&sb, module->funcs[i], module, 0, &tmp);
    }

    /* Copy to malloc'd string for caller */
    const char *content = iron_strbuf_get(&sb);
    size_t len = strlen(content);
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, content, len + 1);
    }

    iron_strbuf_free(&sb);
    iron_arena_free(&tmp);

    return result;
}
