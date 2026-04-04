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

static bool has_warning(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code &&
            g_diags.items[i].level == IRON_DIAG_WARNING)
            return true;
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

/* Make a spawn statement with a body block. */
static Iron_SpawnStmt *make_spawn_stmt(Iron_Arena *a, const char *name,
                                        Iron_Node **body_stmts, int body_count) {
    Iron_Block *body = ARENA_ALLOC(a, Iron_Block);
    body->span       = ts(3, 20);
    body->kind       = IRON_NODE_BLOCK;
    body->stmts      = body_stmts;
    body->stmt_count = body_count;

    Iron_SpawnStmt *ss = ARENA_ALLOC(a, Iron_SpawnStmt);
    ss->span        = ts(3, 3);
    ss->kind        = IRON_NODE_SPAWN;
    ss->name        = name;
    ss->pool_expr   = NULL;
    ss->body        = (Iron_Node *)body;
    ss->handle_name = NULL;
    return ss;
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

/* ── Test 10: spawn block writes outer var => IRON_WARN_SPAWN_DATA_RACE ───── */

void test_spawn_write_outer_var_race(void) {
    /* var total = 0
     * spawn("t1") { total = 1 }    <- writes outer var
     */
    Iron_VarDecl *var_total = make_var_int(&g_arena, "total");

    /* Spawn body: total = 1 */
    Iron_Ident *tgt = make_ident(&g_arena, "total");
    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(4, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(4, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)tgt;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)assign;

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "t1", body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_total;
    fn_stmts[1] = (Iron_Node *)spawn;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_write", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* ── Test 11: spawn block writes outer var via field => IRON_WARN_SPAWN_DATA_RACE ── */

void test_spawn_field_write_outer_race(void) {
    /* var obj = 0
     * spawn("t2") { obj.x = 1 }    <- writes outer via field
     */
    Iron_VarDecl *var_obj = make_var_int(&g_arena, "obj");

    /* Spawn body: obj.x = 1 */
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

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "t2", body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_obj;
    fn_stmts[1] = (Iron_Node *)spawn;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_field_write", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* ── Test 12: spawn block writes outer var via index => IRON_WARN_SPAWN_DATA_RACE ── */

void test_spawn_index_write_outer_race(void) {
    /* var arr = 0
     * spawn("t3") { arr[0] = 1 }    <- writes outer via index
     */
    Iron_VarDecl *var_arr = make_var_int(&g_arena, "arr");

    /* Spawn body: arr[0] = 1 */
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

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "t3", body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_arr;
    fn_stmts[1] = (Iron_Node *)spawn;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_index_write", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* ── Test 13: spawn block reads outer var only => no warning ──────────────── */

void test_spawn_read_outer_var_ok(void) {
    /* val data = 0
     * spawn("t4") { val x = data }    <- read only, no write
     */
    Iron_ValDecl *val_data = make_val_int(&g_arena, "data", 0);

    /* Spawn body: val x = data */
    Iron_Ident *read_id = make_ident(&g_arena, "data");
    Iron_ValDecl *val_x = ARENA_ALLOC(&g_arena, Iron_ValDecl);
    val_x->span         = ts(4, 7);
    val_x->kind         = IRON_NODE_VAL_DECL;
    val_x->name         = "x";
    val_x->type_ann     = NULL;
    val_x->init         = (Iron_Node *)read_id;
    val_x->declared_type = NULL;

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)val_x;

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "t4", body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)val_data;
    fn_stmts[1] = (Iron_Node *)spawn;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_read_ok", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* ── Test 14: spawn block writes only local variable => no warning ────────── */

void test_spawn_write_local_var_ok(void) {
    /* spawn("t5") { var local = 0; local = 1 }    <- local only
     */
    Iron_VarDecl *local_var = make_var_int(&g_arena, "local");
    local_var->span = ts(4, 7);

    /* Assign: local = 1 */
    Iron_Ident *tgt = make_ident(&g_arena, "local");
    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(5, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(5, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)tgt;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **body_stmts = make_stmts(&g_arena, 2);
    body_stmts[0] = (Iron_Node *)local_var;
    body_stmts[1] = (Iron_Node *)assign;

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "t5", body_stmts, 2);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 1);
    fn_stmts[0] = (Iron_Node *)spawn;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_local_ok", fn_stmts, 1);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* ── Edge-case tests (Phase 39, Plan 02) ─────────────────────────────────── */

/* Test 15: Spawn inside sequential for loop, writes outer var => WARN */
void test_spawn_inside_sequential_for(void) {
    /* var x = 0
     * for i in range(10) {        -- sequential for
     *   spawn("s") { x = 1 }      -- writes outer var from spawn
     * }
     */
    Iron_VarDecl *var_x = make_var_int(&g_arena, "x");

    /* Spawn body: x = 1 */
    Iron_Ident *tgt = make_ident(&g_arena, "x");
    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(5, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign->span   = ts(5, 5);
    assign->kind   = IRON_NODE_ASSIGN;
    assign->target = (Iron_Node *)tgt;
    assign->value  = (Iron_Node *)rhs;
    assign->op     = 0;

    Iron_Node **spawn_body = make_stmts(&g_arena, 1);
    spawn_body[0] = (Iron_Node *)assign;

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "s", spawn_body, 1);

    /* Sequential for loop body: spawn(...) */
    Iron_Node **for_body = make_stmts(&g_arena, 1);
    for_body[0] = (Iron_Node *)spawn;

    Iron_ForStmt *for_s = make_for_stmt(&g_arena, false /* sequential */, for_body, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_x;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_in_for", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* Test 16: Multiple spawn blocks sharing same outer variable => WARN */
void test_multiple_spawns_same_var(void) {
    /* var shared = 0
     * spawn("s1") { shared = 1 }
     * spawn("s2") { shared = 2 }
     */
    Iron_VarDecl *var_shared = make_var_int(&g_arena, "shared");

    /* Spawn s1 body: shared = 1 */
    Iron_Ident *tgt1 = make_ident(&g_arena, "shared");
    Iron_IntLit *rhs1 = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs1->span          = ts(4, 15);
    rhs1->kind          = IRON_NODE_INT_LIT;
    rhs1->resolved_type = NULL;
    rhs1->value         = "1";

    Iron_AssignStmt *assign1 = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign1->span   = ts(4, 5);
    assign1->kind   = IRON_NODE_ASSIGN;
    assign1->target = (Iron_Node *)tgt1;
    assign1->value  = (Iron_Node *)rhs1;
    assign1->op     = 0;

    Iron_Node **s1_body = make_stmts(&g_arena, 1);
    s1_body[0] = (Iron_Node *)assign1;
    Iron_SpawnStmt *spawn1 = make_spawn_stmt(&g_arena, "s1", s1_body, 1);

    /* Spawn s2 body: shared = 2 */
    Iron_Ident *tgt2 = make_ident(&g_arena, "shared");
    Iron_IntLit *rhs2 = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs2->span          = ts(5, 15);
    rhs2->kind          = IRON_NODE_INT_LIT;
    rhs2->resolved_type = NULL;
    rhs2->value         = "2";

    Iron_AssignStmt *assign2 = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign2->span   = ts(5, 5);
    assign2->kind   = IRON_NODE_ASSIGN;
    assign2->target = (Iron_Node *)tgt2;
    assign2->value  = (Iron_Node *)rhs2;
    assign2->op     = 0;

    Iron_Node **s2_body = make_stmts(&g_arena, 1);
    s2_body[0] = (Iron_Node *)assign2;
    Iron_SpawnStmt *spawn2 = make_spawn_stmt(&g_arena, "s2", s2_body, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 3);
    fn_stmts[0] = (Iron_Node *)var_shared;
    fn_stmts[1] = (Iron_Node *)spawn1;
    fn_stmts[2] = (Iron_Node *)spawn2;

    Iron_Program *prog   = make_prog(&g_arena, "multi_spawn", fn_stmts, 3);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
}

/* Test 17: Spawn reads var x, different spawn writes var y => only y warns */
void test_spawn_read_write_different_vars(void) {
    /* val x = 0
     * var y = 0
     * spawn("s1") { val z = x }   -- reads x only (val => no write)
     * spawn("s2") { y = 1 }       -- writes y => WARN
     */
    Iron_ValDecl *val_x = make_val_int(&g_arena, "x", 0);
    Iron_VarDecl *var_y = make_var_int(&g_arena, "y");

    /* Spawn s1 body: val z = x (read only) */
    Iron_Ident *read_x = make_ident(&g_arena, "x");
    Iron_ValDecl *val_z = ARENA_ALLOC(&g_arena, Iron_ValDecl);
    val_z->span         = ts(4, 7);
    val_z->kind         = IRON_NODE_VAL_DECL;
    val_z->name         = "z";
    val_z->type_ann     = NULL;
    val_z->init         = (Iron_Node *)read_x;
    val_z->declared_type = NULL;

    Iron_Node **s1_body = make_stmts(&g_arena, 1);
    s1_body[0] = (Iron_Node *)val_z;
    Iron_SpawnStmt *spawn1 = make_spawn_stmt(&g_arena, "s1", s1_body, 1);

    /* Spawn s2 body: y = 1 (write) */
    Iron_Ident *tgt_y = make_ident(&g_arena, "y");
    Iron_IntLit *rhs = ARENA_ALLOC(&g_arena, Iron_IntLit);
    rhs->span          = ts(5, 15);
    rhs->kind          = IRON_NODE_INT_LIT;
    rhs->resolved_type = NULL;
    rhs->value         = "1";

    Iron_AssignStmt *assign_y = ARENA_ALLOC(&g_arena, Iron_AssignStmt);
    assign_y->span   = ts(5, 5);
    assign_y->kind   = IRON_NODE_ASSIGN;
    assign_y->target = (Iron_Node *)tgt_y;
    assign_y->value  = (Iron_Node *)rhs;
    assign_y->op     = 0;

    Iron_Node **s2_body = make_stmts(&g_arena, 1);
    s2_body[0] = (Iron_Node *)assign_y;
    Iron_SpawnStmt *spawn2 = make_spawn_stmt(&g_arena, "s2", s2_body, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 4);
    fn_stmts[0] = (Iron_Node *)val_x;
    fn_stmts[1] = (Iron_Node *)var_y;
    fn_stmts[2] = (Iron_Node *)spawn1;
    fn_stmts[3] = (Iron_Node *)spawn2;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_rw_diff", fn_stmts, 4);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    /* s2 writes y => WARN */
    TEST_ASSERT_TRUE(has_warning(IRON_WARN_SPAWN_DATA_RACE));

    /* Check that the warning mentions 'y', not 'x' */
    bool found_y_warn = false;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == IRON_WARN_SPAWN_DATA_RACE &&
            g_diags.items[i].level == IRON_DIAG_WARNING &&
            strstr(g_diags.items[i].message, "'y'") != NULL) {
            found_y_warn = true;
        }
    }
    TEST_ASSERT_TRUE(found_y_warn);
}

/* Test 18: Parallel-for body with nested sequential for that mutates outer => ERROR */
void test_parallel_for_nested_sequential_for_outer_mutation(void) {
    /* var total = 0
     * for |i| in range(10) {          -- parallel
     *   for j in range(5) {            -- sequential nested
     *     total = 1                     -- outer mutation through nesting
     *   }
     * }
     */
    Iron_VarDecl *var_total = make_var_int(&g_arena, "total");

    /* Inner sequential for body: total = 1 */
    Iron_AssignStmt *incr = make_compound_assign(&g_arena, "total", 0);

    Iron_Node **inner_body = make_stmts(&g_arena, 1);
    inner_body[0] = (Iron_Node *)incr;

    Iron_ForStmt *inner_for = make_for_stmt(&g_arena, false /* sequential */, inner_body, 1);
    inner_for->var_name = "j";

    /* Outer parallel for body: inner sequential for */
    Iron_Node **outer_body = make_stmts(&g_arena, 1);
    outer_body[0] = (Iron_Node *)inner_for;

    Iron_ForStmt *outer_for = make_for_stmt(&g_arena, true /* parallel */, outer_body, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 2);
    fn_stmts[0] = (Iron_Node *)var_total;
    fn_stmts[1] = (Iron_Node *)outer_for;

    Iron_Program *prog   = make_prog(&g_arena, "par_nested_seq_mut", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_TRUE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* Test 19: Parallel-for body only reads outer val => NO error */
void test_parallel_for_read_only_ok(void) {
    /* val limit = 0
     * for |i| in range(10) {
     *   val x = limit            -- read only, OK
     * }
     */
    Iron_ValDecl *val_limit = make_val_int(&g_arena, "limit", 0);

    /* Body: val x = limit */
    Iron_Ident *read_id = make_ident(&g_arena, "limit");
    Iron_ValDecl *val_x = ARENA_ALLOC(&g_arena, Iron_ValDecl);
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
    fn_stmts[0] = (Iron_Node *)val_limit;
    fn_stmts[1] = (Iron_Node *)for_s;

    Iron_Program *prog   = make_prog(&g_arena, "par_read_only", fn_stmts, 2);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_error(IRON_ERR_PARALLEL_MUTATION));
}

/* Test 20: Spawn block with only local declarations => NO warning */
void test_spawn_only_locals_ok(void) {
    /* spawn("clean") { val local = 42 }   -- no outer refs
     */
    Iron_ValDecl *val_local = make_val_int(&g_arena, "local", 1);
    val_local->span = ts(4, 7);

    Iron_Node **body_stmts = make_stmts(&g_arena, 1);
    body_stmts[0] = (Iron_Node *)val_local;

    Iron_SpawnStmt *spawn = make_spawn_stmt(&g_arena, "clean", body_stmts, 1);

    Iron_Node **fn_stmts = make_stmts(&g_arena, 1);
    fn_stmts[0] = (Iron_Node *)spawn;

    Iron_Program *prog   = make_prog(&g_arena, "spawn_locals_only", fn_stmts, 1);
    Iron_Scope   *global = resolve_quiet(prog, &g_arena);

    iron_concurrency_check(prog, global, &g_arena, &g_diags);

    TEST_ASSERT_FALSE(has_warning(IRON_WARN_SPAWN_DATA_RACE));
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

    /* Spawn capture analysis tests */
    RUN_TEST(test_spawn_write_outer_var_race);
    RUN_TEST(test_spawn_field_write_outer_race);
    RUN_TEST(test_spawn_index_write_outer_race);
    RUN_TEST(test_spawn_read_outer_var_ok);
    RUN_TEST(test_spawn_write_local_var_ok);

    /* Edge-case tests (Phase 39, Plan 02) */
    RUN_TEST(test_spawn_inside_sequential_for);
    RUN_TEST(test_multiple_spawns_same_var);
    RUN_TEST(test_spawn_read_write_different_vars);
    RUN_TEST(test_parallel_for_nested_sequential_for_outer_mutation);
    RUN_TEST(test_parallel_for_read_only_ok);
    RUN_TEST(test_spawn_only_locals_ok);

    return UNITY_END();
}
