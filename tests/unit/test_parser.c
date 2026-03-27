#include "unity.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <string.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Parse helper ────────────────────────────────────────────────────────── */

static Iron_Node *parse(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &arena, &diags);
    return iron_parse(&p);
}

static Iron_Node *first_decl(Iron_Node *prog) {
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_NOT_NULL(pr->decls);
    TEST_ASSERT_GREATER_THAN(0, pr->decl_count);
    return pr->decls[0];
}

static Iron_Node *first_stmt_of_func(Iron_Node *decl) {
    Iron_FuncDecl *f    = (Iron_FuncDecl *)decl;
    Iron_Block    *body = (Iron_Block *)f->body;
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_GREATER_THAN(0, body->stmt_count);
    return body->stmts[0];
}

/* ── val / var declarations ──────────────────────────────────────────────── */

void test_parse_val_decl(void) {
    Iron_Node *prog = parse("val x = 10");
    TEST_ASSERT_EQUAL(IRON_NODE_PROGRAM, prog->kind);
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_VAL_DECL, d->kind);
    Iron_ValDecl *v = (Iron_ValDecl *)d;
    TEST_ASSERT_EQUAL_STRING("x", v->name);
    TEST_ASSERT_NULL(v->type_ann);
    TEST_ASSERT_NOT_NULL(v->init);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, v->init->kind);
    TEST_ASSERT_EQUAL_STRING("10", ((Iron_IntLit *)v->init)->value);
}

void test_parse_var_decl(void) {
    Iron_Node *prog = parse("var y: Int = 20");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_VAR_DECL, d->kind);
    Iron_VarDecl *v = (Iron_VarDecl *)d;
    TEST_ASSERT_EQUAL_STRING("y", v->name);
    TEST_ASSERT_NOT_NULL(v->type_ann);
    TEST_ASSERT_EQUAL(IRON_NODE_TYPE_ANNOTATION, v->type_ann->kind);
    TEST_ASSERT_EQUAL_STRING("Int", ((Iron_TypeAnnotation *)v->type_ann)->name);
    TEST_ASSERT_NOT_NULL(v->init);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, v->init->kind);
}

/* ── Function declarations ───────────────────────────────────────────────── */

void test_parse_func_decl(void) {
    Iron_Node *prog = parse("func add(a: Int, b: Int) -> Int { return a + b }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_FUNC_DECL, d->kind);
    Iron_FuncDecl *f = (Iron_FuncDecl *)d;
    TEST_ASSERT_EQUAL_STRING("add", f->name);
    TEST_ASSERT_EQUAL(2, f->param_count);
    TEST_ASSERT_EQUAL_STRING("a", ((Iron_Param *)f->params[0])->name);
    TEST_ASSERT_EQUAL_STRING("b", ((Iron_Param *)f->params[1])->name);
    TEST_ASSERT_NOT_NULL(f->return_type);
    TEST_ASSERT_NOT_NULL(f->body);
    TEST_ASSERT_EQUAL(IRON_NODE_BLOCK, f->body->kind);
}

void test_parse_method_decl(void) {
    Iron_Node *prog = parse("func Player.update(dt: Float) { }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, d->kind);
    Iron_MethodDecl *m = (Iron_MethodDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Player", m->type_name);
    TEST_ASSERT_EQUAL_STRING("update", m->method_name);
    TEST_ASSERT_EQUAL(1, m->param_count);
    TEST_ASSERT_EQUAL_STRING("dt", ((Iron_Param *)m->params[0])->name);
}

/* ── Object declarations ─────────────────────────────────────────────────── */

void test_parse_object_decl(void) {
    Iron_Node *prog = parse("object Player {\n  var pos: Vec2\n  val name: String\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Player", obj->name);
    TEST_ASSERT_EQUAL(2, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    Iron_Field *f1 = (Iron_Field *)obj->fields[1];
    TEST_ASSERT_EQUAL_STRING("pos",  f0->name);
    TEST_ASSERT_EQUAL_STRING("name", f1->name);
}

void test_parse_object_extends(void) {
    Iron_Node *prog = parse("object Player extends Entity {\n  val name: String\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Entity", obj->extends_name);
}

void test_parse_object_implements(void) {
    Iron_Node *prog = parse("object Player implements Drawable, Updatable { }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL(2, obj->implements_count);
    TEST_ASSERT_EQUAL_STRING("Drawable",  obj->implements_names[0]);
    TEST_ASSERT_EQUAL_STRING("Updatable", obj->implements_names[1]);
}

/* ── Interface declarations ──────────────────────────────────────────────── */

void test_parse_interface_decl(void) {
    Iron_Node *prog = parse("interface Drawable {\n  func draw()\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_INTERFACE_DECL, d->kind);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Drawable", iface->name);
    TEST_ASSERT_EQUAL(1, iface->method_count);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_EQUAL_STRING("draw", sig->name);
    TEST_ASSERT_NULL(sig->body);  /* signatures have no body */
}

/* ── Enum declarations ───────────────────────────────────────────────────── */

void test_parse_enum_decl(void) {
    Iron_Node *prog = parse("enum GameState {\n  PAUSED,\n  RUNNING,\n  MENU\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_ENUM_DECL, d->kind);
    Iron_EnumDecl *e = (Iron_EnumDecl *)d;
    TEST_ASSERT_EQUAL_STRING("GameState", e->name);
    TEST_ASSERT_EQUAL(3, e->variant_count);
    TEST_ASSERT_EQUAL_STRING("PAUSED",  ((Iron_EnumVariant *)e->variants[0])->name);
    TEST_ASSERT_EQUAL_STRING("RUNNING", ((Iron_EnumVariant *)e->variants[1])->name);
    TEST_ASSERT_EQUAL_STRING("MENU",    ((Iron_EnumVariant *)e->variants[2])->name);
}

/* ── Import declarations ─────────────────────────────────────────────────── */

void test_parse_import(void) {
    Iron_Node *prog = parse("import player");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_IMPORT_DECL, d->kind);
    Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
    TEST_ASSERT_EQUAL_STRING("player", imp->path);
    TEST_ASSERT_NULL(imp->alias);
}

void test_parse_import_alias(void) {
    Iron_Node *prog = parse("import physics.collision as pc");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_IMPORT_DECL, d->kind);
    Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
    TEST_ASSERT_EQUAL_STRING("physics.collision", imp->path);
    TEST_ASSERT_EQUAL_STRING("pc", imp->alias);
}

/* ── Control flow statements ─────────────────────────────────────────────── */

void test_parse_if_stmt(void) {
    Iron_Node *prog = parse("func f() { if x > 0 { y = 1 } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_IF, stmt->kind);
    Iron_IfStmt *ifs = (Iron_IfStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ifs->condition);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, ifs->condition->kind);
    TEST_ASSERT_NOT_NULL(ifs->body);
}

void test_parse_if_elif_else(void) {
    Iron_Node *prog = parse("func f() { if a { } elif b { } else { } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_IF, stmt->kind);
    Iron_IfStmt *ifs = (Iron_IfStmt *)stmt;
    TEST_ASSERT_EQUAL(1, ifs->elif_count);
    TEST_ASSERT_NOT_NULL(ifs->elif_conds[0]);
    TEST_ASSERT_NOT_NULL(ifs->elif_bodies[0]);
    TEST_ASSERT_NOT_NULL(ifs->else_body);
}

void test_parse_while_stmt(void) {
    Iron_Node *prog = parse("func f() { while running { update() } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_WHILE, stmt->kind);
    Iron_WhileStmt *ws = (Iron_WhileStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ws->condition);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, ws->condition->kind);
    TEST_ASSERT_NOT_NULL(ws->body);
}

void test_parse_for_stmt(void) {
    Iron_Node *prog = parse("func f() { for i in range(10) { } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_FOR, stmt->kind);
    Iron_ForStmt *fs = (Iron_ForStmt *)stmt;
    TEST_ASSERT_EQUAL_STRING("i", fs->var_name);
    TEST_ASSERT_NOT_NULL(fs->iterable);
    TEST_ASSERT_FALSE(fs->is_parallel);
}

void test_parse_for_parallel(void) {
    Iron_Node *prog = parse("func f() { for i in range(10) parallel { } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_FOR, stmt->kind);
    Iron_ForStmt *fs = (Iron_ForStmt *)stmt;
    TEST_ASSERT_TRUE(fs->is_parallel);
    TEST_ASSERT_NULL(fs->pool_expr);
}

void test_parse_match_stmt(void) {
    Iron_Node *prog = parse("func f() { match state {\n  GameState.RUNNING { }\n  else { }\n} }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_MATCH, stmt->kind);
    Iron_MatchStmt *ms = (Iron_MatchStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ms->subject);
    TEST_ASSERT_EQUAL(1, ms->case_count);
    TEST_ASSERT_NOT_NULL(ms->else_body);
}

void test_parse_defer(void) {
    Iron_Node *prog = parse("func f() { defer close(file) }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_DEFER, stmt->kind);
    Iron_DeferStmt *ds = (Iron_DeferStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ds->expr);
}

void test_parse_return(void) {
    Iron_Node *prog = parse("func f() -> Int { return 42 }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, stmt->kind);
    Iron_ReturnStmt *rs = (Iron_ReturnStmt *)stmt;
    TEST_ASSERT_NOT_NULL(rs->value);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, rs->value->kind);
    TEST_ASSERT_EQUAL_STRING("42", ((Iron_IntLit *)rs->value)->value);
}

/* ── Operator precedence ─────────────────────────────────────────────────── */

void test_precedence_add_mul(void) {
    /* 2 + 3 * 4 should be BinaryExpr(+, 2, BinaryExpr(*, 3, 4)) */
    Iron_Node *prog = parse("val r = 2 + 3 * 4");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_VAL_DECL, d->kind);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, init->kind);
    Iron_BinaryExpr *add = (Iron_BinaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_PLUS, add->op);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, add->left->kind);
    TEST_ASSERT_EQUAL_STRING("2", ((Iron_IntLit *)add->left)->value);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, add->right->kind);
    Iron_BinaryExpr *mul = (Iron_BinaryExpr *)add->right;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_STAR, mul->op);
    TEST_ASSERT_EQUAL_STRING("3", ((Iron_IntLit *)mul->left)->value);
    TEST_ASSERT_EQUAL_STRING("4", ((Iron_IntLit *)mul->right)->value);
}

void test_precedence_comparison(void) {
    /* x > 0 and y < 10 -> BinaryExpr(and, BinaryExpr(>, x, 0), BinaryExpr(<, y, 10)) */
    Iron_Node *prog = parse("val r = x > 0 and y < 10");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, init->kind);
    Iron_BinaryExpr *and_node = (Iron_BinaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_AND, and_node->op);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, and_node->left->kind);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, and_node->right->kind);
    Iron_BinaryExpr *gt = (Iron_BinaryExpr *)and_node->left;
    Iron_BinaryExpr *lt = (Iron_BinaryExpr *)and_node->right;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_GREATER, gt->op);
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_LESS,    lt->op);
}

void test_precedence_or_and(void) {
    /* a or b and c -> BinaryExpr(or, a, BinaryExpr(and, b, c)) */
    Iron_Node *prog = parse("val r = a or b and c");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, init->kind);
    Iron_BinaryExpr *or_node = (Iron_BinaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_OR, or_node->op);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, or_node->left->kind);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, or_node->right->kind);
    Iron_BinaryExpr *and_node = (Iron_BinaryExpr *)or_node->right;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_AND, and_node->op);
}

/* ── Unary expressions ───────────────────────────────────────────────────── */

void test_unary_minus(void) {
    Iron_Node *prog = parse("val r = -x");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_UNARY, init->kind);
    Iron_UnaryExpr *u = (Iron_UnaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_MINUS, u->op);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, u->operand->kind);
}

void test_unary_not(void) {
    Iron_Node *prog = parse("val r = not done");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_UNARY, init->kind);
    Iron_UnaryExpr *u = (Iron_UnaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_NOT, u->op);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, u->operand->kind);
    TEST_ASSERT_EQUAL_STRING("done", ((Iron_Ident *)u->operand)->name);
}

/* ── Call expressions ────────────────────────────────────────────────────── */

void test_call_expr(void) {
    Iron_Node *prog = parse("val r = add(1, 2)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_CALL, init->kind);
    Iron_CallExpr *c = (Iron_CallExpr *)init;
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, c->callee->kind);
    TEST_ASSERT_EQUAL(2, c->arg_count);
}

void test_method_call(void) {
    Iron_Node *prog = parse("val r = player.update(dt)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_CALL, init->kind);
    Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)init;
    TEST_ASSERT_EQUAL_STRING("update", mc->method);
    TEST_ASSERT_EQUAL(1, mc->arg_count);
}

void test_field_access(void) {
    Iron_Node *prog = parse("val r = player.hp");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_FIELD_ACCESS, init->kind);
    Iron_FieldAccess *fa = (Iron_FieldAccess *)init;
    TEST_ASSERT_EQUAL_STRING("hp", fa->field);
}

void test_index_access(void) {
    Iron_Node *prog = parse("val r = items[0]");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_INDEX, init->kind);
    Iron_IndexExpr *ix = (Iron_IndexExpr *)init;
    TEST_ASSERT_NOT_NULL(ix->object);
    TEST_ASSERT_NOT_NULL(ix->index);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, ix->index->kind);
}

/* ── Special expressions ─────────────────────────────────────────────────── */

void test_lambda(void) {
    Iron_Node *prog = parse("val r = func(x: Int) -> Int { return x * 2 }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_LAMBDA, init->kind);
    Iron_LambdaExpr *lam = (Iron_LambdaExpr *)init;
    TEST_ASSERT_EQUAL(1, lam->param_count);
    TEST_ASSERT_NOT_NULL(lam->return_type);
    TEST_ASSERT_NOT_NULL(lam->body);
}

void test_heap_expr(void) {
    Iron_Node *prog = parse("val r = heap Enemy(1, 2)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_HEAP, init->kind);
    Iron_HeapExpr *he = (Iron_HeapExpr *)init;
    TEST_ASSERT_NOT_NULL(he->inner);
    /* The inner is a CallExpr since ConstructExpr / CallExpr are the same at parse time */
    TEST_ASSERT_TRUE(he->inner->kind == IRON_NODE_CALL || he->inner->kind == IRON_NODE_CONSTRUCT);
}

void test_is_expr(void) {
    Iron_Node *prog = parse("val r = a is Player");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_IS, init->kind);
    Iron_IsExpr *is_n = (Iron_IsExpr *)init;
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, is_n->expr->kind);
    TEST_ASSERT_EQUAL_STRING("Player", is_n->type_name);
}

void test_construct_expr(void) {
    Iron_Node *prog = parse("val r = Player(pos, 100)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    /* Parser emits CallExpr; semantic analysis disambiguates */
    TEST_ASSERT_TRUE(init->kind == IRON_NODE_CALL || init->kind == IRON_NODE_CONSTRUCT);
    if (init->kind == IRON_NODE_CALL) {
        Iron_CallExpr *c = (Iron_CallExpr *)init;
        TEST_ASSERT_EQUAL(2, c->arg_count);
    }
}

/* ── Span invariant ──────────────────────────────────────────────────────── */

typedef struct {
    bool all_positive;
} SpanCheckCtx;

static bool span_check_visitor(Iron_Visitor *v, Iron_Node *node) {
    SpanCheckCtx *ctx = (SpanCheckCtx *)v->ctx;
    /* Every node must have line >= 1 */
    if (node->span.line == 0) ctx->all_positive = false;
    return true;
}

void test_every_node_has_span(void) {
    Iron_Node *prog = parse(
        "func add(a: Int, b: Int) -> Int {\n"
        "  return a + b\n"
        "}\n"
        "val x = 10\n"
        "var y: Float = 3.14\n"
        "object Foo {\n"
        "  var z: Int\n"
        "}\n"
    );
    SpanCheckCtx ctx = { .all_positive = true };
    Iron_Visitor v   = {
        .ctx        = &ctx,
        .visit_node = span_check_visitor,
        .post_visit = NULL
    };
    iron_ast_walk(prog, &v);
    TEST_ASSERT_TRUE(ctx.all_positive);
}

/* ── Generic declarations ────────────────────────────────────────────────── */

void test_generic_func(void) {
    Iron_Node *prog = parse("func find[T](items: [T]) -> T? { return null }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_FUNC_DECL, d->kind);
    Iron_FuncDecl *f = (Iron_FuncDecl *)d;
    TEST_ASSERT_EQUAL_STRING("find", f->name);
    TEST_ASSERT_EQUAL(1, f->generic_param_count);
    TEST_ASSERT_EQUAL(1, f->param_count);
    TEST_ASSERT_NOT_NULL(f->return_type);
}

void test_generic_object(void) {
    Iron_Node *prog = parse("object Pool[T] {\n  var items: [T]\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Pool", obj->name);
    TEST_ASSERT_EQUAL(1, obj->generic_param_count);
    TEST_ASSERT_EQUAL(1, obj->field_count);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_val_decl);
    RUN_TEST(test_parse_var_decl);
    RUN_TEST(test_parse_func_decl);
    RUN_TEST(test_parse_method_decl);
    RUN_TEST(test_parse_object_decl);
    RUN_TEST(test_parse_object_extends);
    RUN_TEST(test_parse_object_implements);
    RUN_TEST(test_parse_interface_decl);
    RUN_TEST(test_parse_enum_decl);
    RUN_TEST(test_parse_import);
    RUN_TEST(test_parse_import_alias);
    RUN_TEST(test_parse_if_stmt);
    RUN_TEST(test_parse_if_elif_else);
    RUN_TEST(test_parse_while_stmt);
    RUN_TEST(test_parse_for_stmt);
    RUN_TEST(test_parse_for_parallel);
    RUN_TEST(test_parse_match_stmt);
    RUN_TEST(test_parse_defer);
    RUN_TEST(test_parse_return);
    RUN_TEST(test_precedence_add_mul);
    RUN_TEST(test_precedence_comparison);
    RUN_TEST(test_precedence_or_and);
    RUN_TEST(test_unary_minus);
    RUN_TEST(test_unary_not);
    RUN_TEST(test_call_expr);
    RUN_TEST(test_method_call);
    RUN_TEST(test_field_access);
    RUN_TEST(test_index_access);
    RUN_TEST(test_lambda);
    RUN_TEST(test_heap_expr);
    RUN_TEST(test_is_expr);
    RUN_TEST(test_construct_expr);
    RUN_TEST(test_every_node_has_span);
    RUN_TEST(test_generic_func);
    RUN_TEST(test_generic_object);

    return UNITY_END();
}
