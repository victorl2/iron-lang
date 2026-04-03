/* test_concurrency.c — Unity tests for the Iron concurrency checking pass.
 *
 * Tests cover:
 *   - Parallel-for body mutates outer non-mutex var => E0208
 *   - Parallel-for body reads outer variable => no error
 *   - Parallel-for body only uses loop variable => no error
 *   - Parallel-for body mutates local variable => no error
 *   - Sequential for loop with outer mutation => no error
 */

#include "unity.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
#include "analyzer/escape.h"
#include "analyzer/concurrency.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>
#include <stdbool.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 512);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

static bool has_error(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* ── Span helper ─────────────────────────────────────────────────────────── */
static Iron_Span ts(int l, int c) {
    return iron_span_make("test.iron", (uint32_t)l, (uint32_t)c,
                          (uint32_t)l, (uint32_t)(c + 1));
}

/* ── Helpers: quiet resolve+typecheck+escape, then concurrency check ────── */

static Iron_Scope *resolve_quiet(Iron_Program *prog, Iron_Arena *a) {
    Iron_DiagList quiet = iron_diaglist_create();
    Iron_Scope *global  = iron_resolve(prog, a, &quiet);
    iron_typecheck(prog, global, a, &quiet);
    iron_escape_analyze(prog, global, a, &quiet);
    iron_diaglist_free(&quiet);
    return global;
}

/* ── AST builder helpers ─────────────────────────────────────────────────── */

static Iron_Node **make_stmts(Iron_Arena *a, int n) {
    return iron_arena_alloc(a, (size_t)n * sizeof(Iron_Node *), _Alignof(Iron_Node *));
}

static Iron_ValDecl *make_val_int(Iron_Arena *a, const char *name, int value) {
    Iron_IntLit *lit = ARENA_ALLOC(a, Iron_IntLit);
    lit->span          = ts(2, 11);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = NULL;
    lit->value         = (value == 0) ? "0" : "1";

    Iron_ValDecl *vd = ARENA_ALLOC(a, Iron_ValDecl);
    vd->span         = ts(2, 3);
    vd->kind         = IRON_NODE_VAL_DECL;
    vd->name         = name;
    vd->type_ann     = NULL;
    vd->init         = (Iron_Node *)lit;
    vd->declared_type = NULL;
    return vd;
}

static Iron_VarDecl *make_var_int(Iron_Arena *a, const char *name) {
    Iron_IntLit *lit = ARENA_ALLOC(a, Iron_IntLit);
    lit->span          = ts(2, 11);
    lit->kind          = IRON_NODE_INT_LIT;
    lit->resolved_type = NULL;
    lit->value         = "0";

    Iron_VarDecl *vd = ARENA_ALLOC(a, Iron_VarDecl);
    vd->span         = ts(2, 3);
    vd->kind         = IRON_NODE_VAR_DECL;
    vd->name         = name;
    vd->type_ann     = NULL;
    vd->init         = (Iron_Node *)lit;
    vd->declared_type = NULL;
    return vd;
}

static Iron_Ident *make_ident(Iron_Arena *a, const char *name) {
    Iron_Ident *id = ARENA_ALLOC(a, Iron_Ident);
    id->span          = ts(3, 10);
    id->kind          = IRON_NODE_IDENT;
    id->resolved_type = NULL;
    id->name          = name;
    id->resolved_sym  = NULL;
    return id;
}

static Iron_FieldAccess *make_field_access(Iron_Arena *a, Iron_Node *object,
                                            const char *field_name) {
    Iron_FieldAccess *fa = ARENA_ALLOC(a, Iron_FieldAccess);
    fa->span          = ts(4, 5);
    fa->kind          = IRON_NODE_FIELD_ACCESS;
    fa->resolved_type = NULL;
    fa->object        = object;
    fa->field         = field_name;
    return fa;
}

static Iron_IndexExpr *make_index_expr(Iron_Arena *a, Iron_Node *object,
                                        Iron_Node *index) {
    Iron_IndexExpr *ie = ARENA_ALLOC(a, Iron_IndexExpr);
    ie->span          = ts(4, 5);
    ie->kind          = IRON_NODE_INDEX;
    ie->resolved_type = NULL;
    ie->object        = object;
    ie->index         = index;
    return ie;
}

/* Make compound-assign: target += rhs_lit */
static Iron_AssignStmt *make_compound_assign(Iron_Arena *a, const char *target_name,
                                              int op_kind) {
    Iron_Ident *tgt = make_ident(a, target_name);
    tgt->span = ts(4, 5);

    Iron_IntLit *rhs = ARENA_ALLOC(a, Iron_IntLit);
    rhs->span          = ts(4, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *as = ARENA_ALLOC(a, Iron_AssignStmt);
    as->span   = ts(4, 5);
    as->kind   = IRON_NODE_ASSIGN;
    as->target = (Iron_Node *)tgt;
    as->value  = (Iron_Node *)rhs;
    as->op     = op_kind;
    return as;
}

/* Make a for-loop (parallel or sequential) with a body block. */
static Iron_ForStmt *make_for_stmt(Iron_Arena *a, bool is_parallel,
                                    Iron_Node **body_stmts, int body_count) {
    Iron_Block *body = ARENA_ALLOC(a, Iron_Block);
    body->span       = ts(3, 20);
    body->kind       = IRON_NODE_BLOCK;
    body->stmts      = body_stmts;
    body->stmt_count = body_count;

    /* Iterable: a call expression "range(10)" — just use a placeholder ident */
    Iron_Ident *iter = make_ident(a, "range_result");
    iter->span = ts(3, 15);

    Iron_ForStmt *fs = ARENA_ALLOC(a, Iron_ForStmt);
    fs->span        = ts(3, 3);
    fs->kind        = IRON_NODE_FOR;
    fs->var_name    = "i";
    fs->iterable    = (Iron_Node *)iter;
    fs->body        = (Iron_Node *)body;
    fs->is_parallel = is_parallel;
    fs->pool_expr   = NULL;
    return fs;
}

/* Wrap stmts in a function and a program. */
static Iron_Program *make_prog(Iron_Arena *a, const char *fn_name,
                                Iron_Node **stmts, int stmt_count) {
    Iron_Block *body = ARENA_ALLOC(a, Iron_Block);
    body->span       = ts(1, 14);
    body->kind       = IRON_NODE_BLOCK;
    body->stmts      = stmts;
    body->stmt_count = stmt_count;

    Iron_FuncDecl *fn = ARENA_ALLOC(a, Iron_FuncDecl);
    fn->span        = ts(1, 1);
    fn->kind        = IRON_NODE_FUNC_DECL;
    fn->name        = fn_name;
    fn->params      = NULL;
    fn->param_count = 0;
    fn->return_type = NULL;
    fn->body        = (Iron_Node *)body;
    fn->is_private  = false;
    fn->generic_params      = NULL;
    fn->generic_param_count = 0;
    fn->resolved_return_type = NULL;

    Iron_Node **decls = iron_arena_alloc(a, sizeof(Iron_Node *), _Alignof(Iron_Node *));
    decls[0] = (Iron_Node *)fn;

    Iron_Program *prog = ARENA_ALLOC(a, Iron_Program);
    prog->span       = ts(1, 1);
    prog->kind       = IRON_NODE_PROGRAM;
    prog->decls      = decls;
    prog->decl_count = 1;
    return prog;
}

/* ── Test 1: parallel-for body mutates outer mutable var => E0208 ─────────── */

void test_parallel_for_outer_mutation_error(void) {
    /* var total = 0
     * for i in range(10) parallel {
     *   total += 1      <- ERROR: mutates outer var
     * }
     */
    Iron_VarDecl *var_total = make_var_int(&g_arena, "total");

    /* Body of parallel-for: total += 1 */
    Iron_AssignStmt *incr = make_compound_assign(&g_arena, "total", 0);

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)incr;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_total;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_mutation", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 2: parallel-for body reads outer val => no error ───────────────── */

void test_parallel_for_outer_read_ok(void) {
    /* val arr = 0
     * for i in range(3) parallel {
     *   val x = arr     <- read only — OK
     * }
     */
    Iron_ValDecl *val_arr = make_val_int(&g_arena, "arr", 0);

    Iron_Ident   *read_id = make_ident(&g_arena, "arr");
    Iron_ValDecl *val_x   = ARENA_ALLOC(&g_arena, Iron_ValDecl);
    val_x->span         = ts(4, 7);
    val_x->kind         = IRON_NODE_VAL_DECL;
    val_x->name         = "x";
    val_x->type_ann     = NULL;
    val_x->init         = (Iron_Node *)read_id;
    val_x->declared_type = NULL;

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)val_x;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)val_arr;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_read_ok", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 3: parallel-for body only uses local variable => no error ────────── */

void test_parallel_for_local_mutation_ok(void) {
    /* for i in range(10) parallel {
     *   var x = 0
     *   x += 1      <- local var only, no outer mutation
     * }
     */
    Iron_VarDecl *local_x = make_var_int(&g_arena, "local_x");
    local_x->span = ts(4, 7);

    Iron_AssignStmt *incr = make_compound_assign(&g_arena, "local_x", 0);

    Iron_Node **body_stmts = make_stmts(&g_arena, 2);
    body_stmts[0] = (Iron_Node *)local_x;
    body_stmts[1] = (Iron_Node *)incr;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 2);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 1);
    fn_stmts[0] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_local_ok", fn_stmts, 1);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 4: sequential for-loop with outer mutation => no error ─────────── */

void test_sequential_for_outer_mutation_ok(void) {
    /* var total = 0
     * for i in range(10) {   -- NOT parallel
     *   total += 1     <- sequential, allowed
     * }
     */
    Iron_VarDecl *var_total = make_var_int(&g_arena, "total2");

    Iron_AssignStmt *incr = make_compound_assign(&g_arena, "total2", 0);

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)incr;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, false /* NOT parallel */,
                                         body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_total;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "seq_ok", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 5: parallel-for body uses loop variable (no outer mutation) ────── */

void test_parallel_for_loop_var_ok(void) {
    /* for i in range(10) parallel {
     *   val x = 1     <- no outer mutation
     * }
     */
    Iron_ValDecl *val_x = make_val_int(&g_arena, "x", 1);
    val_x->span = ts(4, 7);

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)val_x;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 1);
    fn_stmts[0] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_loopvar_ok", fn_stmts, 1);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 6: parallel-for body mutates outer var via field access => E0208 ── */

void test_parallel_for_field_mutation_error(void) {
    /* var obj = ...
     * for i in range(10) parallel {
     *   obj.x = 1      <- ERROR: mutates outer var through field access
     * }
     */
    Iron_VarDecl *var_obj = make_var_int(&g_arena, "obj");

    /* Body: obj.x = 1 */
    Iron_Ident *obj_id = make_ident(&g_arena, "obj");
    Iron_FieldAccess *fa = make_field_access(&g_arena, (Iron_Node *)obj_id, "x");

    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(4, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(4, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)fa;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)assign;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_obj;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_field_mut", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 7: parallel-for body mutates outer var via index => E0208 ──────── */

void test_parallel_for_index_mutation_error(void) {
    /* var arr = ...
     * for i in range(10) parallel {
     *   arr[0] = 1      <- ERROR: mutates outer var through index
     * }
     */
    Iron_VarDecl *var_arr = make_var_int(&g_arena, "arr");

    /* Body: arr[0] = 1 */
    Iron_Ident *arr_id = make_ident(&g_arena, "arr");

    Iron_IntLit *idx = ARENA_ALLOC(&g_arena, Iron_IntLit);
    idx->span          = ts(4, 8);
    idx->kind          = IRON_NODE_INT_LIT;
    idx->resolved_type = NULL;
    idx->value         = "0";

    Iron_IndexExpr *ie = make_index_expr(&g_arena, (Iron_Node *)arr_id,
                                          (Iron_Node *)idx);

    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(4, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(4, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)ie;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)assign;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_arr;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_index_mut", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 8: parallel-for body mutates local var via field => no E0208 ───── */

void test_parallel_for_local_field_mutation_ok(void) {
    /* for i in range(10) parallel {
     *   var local_s = 0
     *   local_s.x = 1     <- local var, no outer mutation
     * }
     */
    Iron_VarDecl *local_s = make_var_int(&g_arena, "local_s");
    local_s->span = ts(4, 7);

    Iron_Ident *ls_id = make_ident(&g_arena, "local_s");
    Iron_FieldAccess *fa = make_field_access(&g_arena, (Iron_Node *)ls_id, "x");

    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(5, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(5, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)fa;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **body_stmts = make_stmts(&g_arena, 2);
    body_stmts[0] = (Iron_Node *)local_s;
    body_stmts[1] = (Iron_Node *)assign;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 2);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 1);
    fn_stmts[0] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_local_field_ok", fn_stmts, 1);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── Test 9: parallel-for body mutates local var via index => no E0208 ───── */

void test_parallel_for_partitioned_local_index_ok(void) {
    /* for i in range(10) parallel {
     *   var local_a = 0
     *   local_a[i] = 1     <- local var, no outer mutation
     * }
     */
    Iron_VarDecl *local_a = make_var_int(&g_arena, "local_a");
    local_a->span = ts(4, 7);

    Iron_Ident *la_id = make_ident(&g_arena, "local_a");
    Iron_Ident *idx_i = make_ident(&g_arena, "i");

    Iron_IndexExpr *ie = make_index_expr(&g_arena, (Iron_Node *)la_id,
                                          (Iron_Node *)idx_i);

    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(5, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(5, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)ie;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **body_stmts = make_stmts(&g_arena, 2);
    body_stmts[0] = (Iron_Node *)local_a;
    body_stmts[1] = (Iron_Node *)assign;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, true, body_stmts, 2);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 1);
    fn_stmts[0] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_local_index_ok", fn_stmts, 1);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parallel_for_outer_mutation_error);
    RUN_TEST(test_parallel_for_outer_read_ok);
    RUN_TEST(test_parallel_for_local_mutation_ok);
    RUN_TEST(test_sequential_for_outer_mutation_ok);
    RUN_TEST(test_parallel_for_loop_var_ok);
    RUN_TEST(test_parallel_for_field_mutation_error);
    RUN_TEST(test_parallel_for_index_mutation_error);
    RUN_TEST(test_parallel_for_local_field_mutation_ok);
    RUN_TEST(test_parallel_for_partitioned_local_index_ok);

    return UNITY_END();
}
