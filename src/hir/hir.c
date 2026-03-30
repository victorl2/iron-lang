#include "hir/hir.h"
#include <stdlib.h>
#include <string.h>

/* Initial arena size for a HIR module: 64 KB */
#define HIR_ARENA_INITIAL_SIZE (64 * 1024)

/* ── Module ──────────────────────────────────────────────────────────────── */

IronHIR_Module *iron_hir_module_create(const char *name) {
    /* Allocate arena on the heap and bootstrap it */
    Iron_Arena *arena = (Iron_Arena *)malloc(sizeof(Iron_Arena));
    *arena = iron_arena_create(HIR_ARENA_INITIAL_SIZE);

    IronHIR_Module *mod = ARENA_ALLOC(arena, IronHIR_Module);
    memset(mod, 0, sizeof(*mod));
    mod->name       = name;
    mod->arena      = arena;
    mod->next_var_id = 1; /* 0 is IRON_HIR_VAR_INVALID sentinel */
    mod->funcs      = NULL;
    mod->func_count = 0;
    mod->name_table = NULL;

    /* Push sentinel at index 0 so indexing by VarId works directly */
    IronHIR_VarInfo sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    sentinel.id   = IRON_HIR_VAR_INVALID;
    sentinel.name = "<invalid>";
    arrput(mod->name_table, sentinel);

    return mod;
}

void iron_hir_module_destroy(IronHIR_Module *mod) {
    if (!mod) return;
    arrfree(mod->funcs);
    arrfree(mod->name_table);
    Iron_Arena *arena = mod->arena;
    iron_arena_free(arena);
    free(arena);
}

/* ── Variable allocation ─────────────────────────────────────────────────── */

IronHIR_VarId iron_hir_alloc_var(IronHIR_Module *mod, const char *name,
                                   Iron_Type *type, bool is_mutable) {
    IronHIR_VarId id = mod->next_var_id++;
    IronHIR_VarInfo info;
    info.id         = id;
    info.name       = name;
    info.type       = type;
    info.is_mutable = is_mutable;
    arrput(mod->name_table, info);
    return id;
}

const char *iron_hir_var_name(IronHIR_Module *mod, IronHIR_VarId id) {
    if (id == IRON_HIR_VAR_INVALID) return "<invalid>";
    if ((ptrdiff_t)id >= arrlen(mod->name_table)) return "<unknown>";
    return mod->name_table[id].name;
}

/* ── Function ────────────────────────────────────────────────────────────── */

IronHIR_Func *iron_hir_func_create(IronHIR_Module *mod, const char *name,
                                    IronHIR_Param *params, int param_count,
                                    Iron_Type *return_type) {
    IronHIR_Func *fn = ARENA_ALLOC(mod->arena, IronHIR_Func);
    memset(fn, 0, sizeof(*fn));
    fn->name        = name;
    fn->return_type = return_type;
    fn->params      = params;
    fn->param_count = param_count;
    fn->body        = NULL;
    return fn;
}

void iron_hir_module_add_func(IronHIR_Module *mod, IronHIR_Func *func) {
    arrput(mod->funcs, func);
    mod->func_count = (int)arrlen(mod->funcs);
}

/* ── Block ───────────────────────────────────────────────────────────────── */

IronHIR_Block *iron_hir_block_create(IronHIR_Module *mod) {
    IronHIR_Block *block = ARENA_ALLOC(mod->arena, IronHIR_Block);
    memset(block, 0, sizeof(*block));
    block->stmts      = NULL;
    block->stmt_count = 0;
    return block;
}

void iron_hir_block_add_stmt(IronHIR_Block *block, IronHIR_Stmt *stmt) {
    arrput(block->stmts, stmt);
    block->stmt_count = (int)arrlen(block->stmts);
}

/* ── Statement constructors ──────────────────────────────────────────────── */

IronHIR_Stmt *iron_hir_stmt_let(IronHIR_Module *mod, IronHIR_VarId var_id,
                                  Iron_Type *type, IronHIR_Expr *init,
                                  bool is_mutable, Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind            = IRON_HIR_STMT_LET;
    s->span            = span;
    s->let.var_id      = var_id;
    s->let.type        = type;
    s->let.init        = init;
    s->let.is_mutable  = is_mutable;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_assign(IronHIR_Module *mod, IronHIR_Expr *target,
                                     IronHIR_Expr *value, Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind          = IRON_HIR_STMT_ASSIGN;
    s->span          = span;
    s->assign.target = target;
    s->assign.value  = value;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_if(IronHIR_Module *mod, IronHIR_Expr *condition,
                                 IronHIR_Block *then_body, IronHIR_Block *else_body,
                                 Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind               = IRON_HIR_STMT_IF;
    s->span               = span;
    s->if_else.condition  = condition;
    s->if_else.then_body  = then_body;
    s->if_else.else_body  = else_body;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_while(IronHIR_Module *mod, IronHIR_Expr *condition,
                                    IronHIR_Block *body, Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind                = IRON_HIR_STMT_WHILE;
    s->span                = span;
    s->while_loop.condition = condition;
    s->while_loop.body      = body;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_for(IronHIR_Module *mod, IronHIR_VarId var_id,
                                  IronHIR_Expr *iterable, IronHIR_Block *body,
                                  Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind            = IRON_HIR_STMT_FOR;
    s->span            = span;
    s->for_loop.var_id   = var_id;
    s->for_loop.iterable = iterable;
    s->for_loop.body     = body;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_match(IronHIR_Module *mod, IronHIR_Expr *scrutinee,
                                    IronHIR_MatchArm *arms, int arm_count,
                                    Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind                 = IRON_HIR_STMT_MATCH;
    s->span                 = span;
    s->match_stmt.scrutinee = scrutinee;
    s->match_stmt.arms      = arms;
    s->match_stmt.arm_count = arm_count;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_return(IronHIR_Module *mod, IronHIR_Expr *value,
                                     Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind               = IRON_HIR_STMT_RETURN;
    s->span               = span;
    s->return_stmt.value  = value;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_defer(IronHIR_Module *mod, IronHIR_Block *body,
                                    Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind       = IRON_HIR_STMT_DEFER;
    s->span       = span;
    s->defer.body = body;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_block(IronHIR_Module *mod, IronHIR_Block *block,
                                    Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind        = IRON_HIR_STMT_BLOCK;
    s->span        = span;
    s->block.block = block;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_expr(IronHIR_Module *mod, IronHIR_Expr *expr,
                                   Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind           = IRON_HIR_STMT_EXPR;
    s->span           = span;
    s->expr_stmt.expr = expr;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_free(IronHIR_Module *mod, IronHIR_Expr *value,
                                   Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind            = IRON_HIR_STMT_FREE;
    s->span            = span;
    s->free_stmt.value = value;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_spawn(IronHIR_Module *mod, const char *handle_name,
                                    IronHIR_Block *body, Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind               = IRON_HIR_STMT_SPAWN;
    s->span               = span;
    s->spawn.handle_name  = handle_name;
    s->spawn.body         = body;
    return s;
}

IronHIR_Stmt *iron_hir_stmt_leak(IronHIR_Module *mod, IronHIR_Expr *value,
                                   Iron_Span span) {
    IronHIR_Stmt *s = ARENA_ALLOC(mod->arena, IronHIR_Stmt);
    memset(s, 0, sizeof(*s));
    s->kind        = IRON_HIR_STMT_LEAK;
    s->span        = span;
    s->leak.value  = value;
    return s;
}

/* ── Expression constructors ─────────────────────────────────────────────── */

IronHIR_Expr *iron_hir_expr_int_lit(IronHIR_Module *mod, int64_t value,
                                      Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind          = IRON_HIR_EXPR_INT_LIT;
    e->span          = span;
    e->type          = type;
    e->int_lit.value = value;
    return e;
}

IronHIR_Expr *iron_hir_expr_float_lit(IronHIR_Module *mod, double value,
                                        Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind            = IRON_HIR_EXPR_FLOAT_LIT;
    e->span            = span;
    e->type            = type;
    e->float_lit.value = value;
    return e;
}

IronHIR_Expr *iron_hir_expr_string_lit(IronHIR_Module *mod, const char *value,
                                         Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind             = IRON_HIR_EXPR_STRING_LIT;
    e->span             = span;
    e->type             = type;
    e->string_lit.value = value;
    return e;
}

IronHIR_Expr *iron_hir_expr_interp_string(IronHIR_Module *mod,
                                            IronHIR_Expr **parts, int part_count,
                                            Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                    = IRON_HIR_EXPR_INTERP_STRING;
    e->span                    = span;
    e->type                    = type;
    e->interp_string.parts      = parts;
    e->interp_string.part_count = part_count;
    return e;
}

IronHIR_Expr *iron_hir_expr_bool_lit(IronHIR_Module *mod, bool value,
                                       Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind           = IRON_HIR_EXPR_BOOL_LIT;
    e->span           = span;
    e->type           = type;
    e->bool_lit.value = value;
    return e;
}

IronHIR_Expr *iron_hir_expr_null_lit(IronHIR_Module *mod, Iron_Type *type,
                                       Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind = IRON_HIR_EXPR_NULL_LIT;
    e->span = span;
    e->type = type;
    return e;
}

IronHIR_Expr *iron_hir_expr_ident(IronHIR_Module *mod, IronHIR_VarId var_id,
                                    const char *name, Iron_Type *type,
                                    Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind           = IRON_HIR_EXPR_IDENT;
    e->span           = span;
    e->type           = type;
    e->ident.var_id   = var_id;
    e->ident.name     = name;
    return e;
}

IronHIR_Expr *iron_hir_expr_binop(IronHIR_Module *mod, IronHIR_BinOp op,
                                    IronHIR_Expr *left, IronHIR_Expr *right,
                                    Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind        = IRON_HIR_EXPR_BINOP;
    e->span        = span;
    e->type        = type;
    e->binop.op    = op;
    e->binop.left  = left;
    e->binop.right = right;
    return e;
}

IronHIR_Expr *iron_hir_expr_unop(IronHIR_Module *mod, IronHIR_UnOp op,
                                   IronHIR_Expr *operand,
                                   Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind          = IRON_HIR_EXPR_UNOP;
    e->span          = span;
    e->type          = type;
    e->unop.op       = op;
    e->unop.operand  = operand;
    return e;
}

IronHIR_Expr *iron_hir_expr_call(IronHIR_Module *mod, IronHIR_Expr *callee,
                                   IronHIR_Expr **args, int arg_count,
                                   Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind           = IRON_HIR_EXPR_CALL;
    e->span           = span;
    e->type           = type;
    e->call.callee    = callee;
    e->call.args      = args;
    e->call.arg_count = arg_count;
    return e;
}

IronHIR_Expr *iron_hir_expr_method_call(IronHIR_Module *mod, IronHIR_Expr *object,
                                          const char *method,
                                          IronHIR_Expr **args, int arg_count,
                                          Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                   = IRON_HIR_EXPR_METHOD_CALL;
    e->span                   = span;
    e->type                   = type;
    e->method_call.object     = object;
    e->method_call.method     = method;
    e->method_call.args       = args;
    e->method_call.arg_count  = arg_count;
    return e;
}

IronHIR_Expr *iron_hir_expr_field_access(IronHIR_Module *mod, IronHIR_Expr *object,
                                           const char *field,
                                           Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                = IRON_HIR_EXPR_FIELD_ACCESS;
    e->span                = span;
    e->type                = type;
    e->field_access.object = object;
    e->field_access.field  = field;
    return e;
}

IronHIR_Expr *iron_hir_expr_index(IronHIR_Module *mod, IronHIR_Expr *array,
                                    IronHIR_Expr *index,
                                    Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind        = IRON_HIR_EXPR_INDEX;
    e->span        = span;
    e->type        = type;
    e->index.array = array;
    e->index.index = index;
    return e;
}

IronHIR_Expr *iron_hir_expr_slice(IronHIR_Module *mod, IronHIR_Expr *array,
                                    IronHIR_Expr *start, IronHIR_Expr *end,
                                    Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind        = IRON_HIR_EXPR_SLICE;
    e->span        = span;
    e->type        = type;
    e->slice.array = array;
    e->slice.start = start;
    e->slice.end   = end;
    return e;
}

IronHIR_Expr *iron_hir_expr_closure(IronHIR_Module *mod,
                                      IronHIR_Param *params, int param_count,
                                      Iron_Type *return_type, IronHIR_Block *body,
                                      Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                   = IRON_HIR_EXPR_CLOSURE;
    e->span                   = span;
    e->type                   = type;
    e->closure.params         = params;
    e->closure.param_count    = param_count;
    e->closure.return_type    = return_type;
    e->closure.body           = body;
    return e;
}

IronHIR_Expr *iron_hir_expr_heap(IronHIR_Module *mod, IronHIR_Expr *inner,
                                   bool auto_free, bool escapes,
                                   Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind           = IRON_HIR_EXPR_HEAP;
    e->span           = span;
    e->type           = type;
    e->heap.inner     = inner;
    e->heap.auto_free = auto_free;
    e->heap.escapes   = escapes;
    return e;
}

IronHIR_Expr *iron_hir_expr_rc(IronHIR_Module *mod, IronHIR_Expr *inner,
                                 Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind     = IRON_HIR_EXPR_RC;
    e->span     = span;
    e->type     = type;
    e->rc.inner = inner;
    return e;
}

IronHIR_Expr *iron_hir_expr_construct(IronHIR_Module *mod, Iron_Type *type,
                                        const char **field_names,
                                        IronHIR_Expr **field_values, int field_count,
                                        Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                   = IRON_HIR_EXPR_CONSTRUCT;
    e->span                   = span;
    e->type                   = type;
    e->construct.type         = type;
    e->construct.field_names  = field_names;
    e->construct.field_values = field_values;
    e->construct.field_count  = field_count;
    return e;
}

IronHIR_Expr *iron_hir_expr_array_lit(IronHIR_Module *mod, Iron_Type *elem_type,
                                        IronHIR_Expr **elements, int element_count,
                                        Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                     = IRON_HIR_EXPR_ARRAY_LIT;
    e->span                     = span;
    e->type                     = type;
    e->array_lit.elem_type      = elem_type;
    e->array_lit.elements       = elements;
    e->array_lit.element_count  = element_count;
    return e;
}

IronHIR_Expr *iron_hir_expr_await(IronHIR_Module *mod, IronHIR_Expr *handle,
                                    Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind             = IRON_HIR_EXPR_AWAIT;
    e->span             = span;
    e->type             = type;
    e->await_expr.handle = handle;
    return e;
}

IronHIR_Expr *iron_hir_expr_cast(IronHIR_Module *mod, IronHIR_Expr *value,
                                   Iron_Type *target_type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind              = IRON_HIR_EXPR_CAST;
    e->span              = span;
    e->type              = target_type;
    e->cast.value        = value;
    e->cast.target_type  = target_type;
    return e;
}

IronHIR_Expr *iron_hir_expr_is_null(IronHIR_Module *mod, IronHIR_Expr *value,
                                      Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind             = IRON_HIR_EXPR_IS_NULL;
    e->span             = span;
    e->type             = NULL;
    e->null_check.value = value;
    return e;
}

IronHIR_Expr *iron_hir_expr_is_not_null(IronHIR_Module *mod, IronHIR_Expr *value,
                                          Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind             = IRON_HIR_EXPR_IS_NOT_NULL;
    e->span             = span;
    e->type             = NULL;
    e->null_check.value = value;
    return e;
}

IronHIR_Expr *iron_hir_expr_func_ref(IronHIR_Module *mod, const char *func_name,
                                       Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                = IRON_HIR_EXPR_FUNC_REF;
    e->span                = span;
    e->type                = type;
    e->func_ref.func_name  = func_name;
    return e;
}

IronHIR_Expr *iron_hir_expr_parallel_for(IronHIR_Module *mod,
                                           IronHIR_VarId var_id,
                                           IronHIR_Expr *range,
                                           IronHIR_Block *body,
                                           Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                 = IRON_HIR_EXPR_PARALLEL_FOR;
    e->span                 = span;
    e->type                 = type;
    e->parallel_for.var_id  = var_id;
    e->parallel_for.range   = range;
    e->parallel_for.body    = body;
    return e;
}

IronHIR_Expr *iron_hir_expr_comptime(IronHIR_Module *mod, IronHIR_Expr *inner,
                                       Iron_Type *type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind          = IRON_HIR_EXPR_COMPTIME;
    e->span          = span;
    e->type          = type;
    e->comptime.inner = inner;
    return e;
}

IronHIR_Expr *iron_hir_expr_is(IronHIR_Module *mod, IronHIR_Expr *value,
                                 Iron_Type *check_type, Iron_Span span) {
    IronHIR_Expr *e = ARENA_ALLOC(mod->arena, IronHIR_Expr);
    memset(e, 0, sizeof(*e));
    e->kind                = IRON_HIR_EXPR_IS;
    e->span                = span;
    e->type                = NULL;
    e->is_check.value      = value;
    e->is_check.check_type = check_type;
    return e;
}
