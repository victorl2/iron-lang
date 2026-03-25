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

/* Get the first expression from a top-level val declaration */
static Iron_Node *parse_expr(const char *src) {
    /* Wrap source in "val _ = <src>" so the expression is parsed */
    char buf[512];
    snprintf(buf, sizeof(buf), "val _ = %s", src);
    Iron_Node *prog = parse(buf);
    if (!prog || prog->kind != IRON_NODE_PROGRAM) return NULL;
    Iron_Program *pr = (Iron_Program *)prog;
    if (pr->decl_count < 1) return NULL;
    Iron_Node *d = pr->decls[0];
    if (d->kind != IRON_NODE_VAL_DECL) return NULL;
    return ((Iron_ValDecl *)d)->init;
}

/* ── String interpolation tests ─────────────────────────────────────────── */

/* "Hello {name}" → InterpString with 2 parts: StringLit("Hello "), Ident("name") */
void test_simple_interp(void) {
    Iron_Node *n = parse_expr("\"Hello {name}\"");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INTERP_STRING, n->kind);

    Iron_InterpString *is = (Iron_InterpString *)n;
    TEST_ASSERT_EQUAL_INT(2, is->part_count);

    /* parts[0]: StringLit("Hello ") */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, is->parts[0]->kind);
    TEST_ASSERT_EQUAL_STRING("Hello ", ((Iron_StringLit *)is->parts[0])->value);

    /* parts[1]: Ident("name") */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, is->parts[1]->kind);
    TEST_ASSERT_EQUAL_STRING("name", ((Iron_Ident *)is->parts[1])->name);
}

/* "{a + b}" → InterpString with 1 part: BinaryExpr */
void test_interp_with_expression(void) {
    Iron_Node *n = parse_expr("\"{a + b}\"");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INTERP_STRING, n->kind);

    Iron_InterpString *is = (Iron_InterpString *)n;
    TEST_ASSERT_EQUAL_INT(1, is->part_count);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_BINARY, is->parts[0]->kind);
}

/* "{x} and {y}" → InterpString with 3 parts: Ident(x), StringLit(" and "), Ident(y) */
void test_interp_multiple(void) {
    Iron_Node *n = parse_expr("\"{x} and {y}\"");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INTERP_STRING, n->kind);

    Iron_InterpString *is = (Iron_InterpString *)n;
    TEST_ASSERT_EQUAL_INT(3, is->part_count);

    /* parts[0]: Ident("x") */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, is->parts[0]->kind);
    /* parts[1]: StringLit(" and ") */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, is->parts[1]->kind);
    TEST_ASSERT_EQUAL_STRING(" and ", ((Iron_StringLit *)is->parts[1])->value);
    /* parts[2]: Ident("y") */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, is->parts[2]->kind);
}

/* "prefix {x} suffix" → 3 parts */
void test_interp_prefix_and_suffix(void) {
    Iron_Node *n = parse_expr("\"prefix {x} suffix\"");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INTERP_STRING, n->kind);

    Iron_InterpString *is = (Iron_InterpString *)n;
    TEST_ASSERT_EQUAL_INT(3, is->part_count);

    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, is->parts[0]->kind);
    TEST_ASSERT_EQUAL_STRING("prefix ", ((Iron_StringLit *)is->parts[0])->value);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, is->parts[1]->kind);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, is->parts[2]->kind);
    TEST_ASSERT_EQUAL_STRING(" suffix", ((Iron_StringLit *)is->parts[2])->value);
}

/* "hello world" (no interpolation) → StringLit, NOT InterpString */
void test_no_interp_plain_string(void) {
    Iron_Node *n = parse_expr("\"hello world\"");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_STRING_LIT, n->kind);
}

/* "{foo(x)}" → InterpString with a CallExpr part (tests nested parens) */
void test_interp_nested_braces(void) {
    Iron_Node *n = parse_expr("\"{foo(x)}\"");
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_INTERP_STRING, n->kind);

    Iron_InterpString *is = (Iron_InterpString *)n;
    TEST_ASSERT_EQUAL_INT(1, is->part_count);
    /* foo(x) is a call expression */
    TEST_ASSERT_EQUAL_INT(IRON_NODE_CALL, is->parts[0]->kind);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_interp);
    RUN_TEST(test_interp_with_expression);
    RUN_TEST(test_interp_multiple);
    RUN_TEST(test_interp_prefix_and_suffix);
    RUN_TEST(test_no_interp_plain_string);
    RUN_TEST(test_interp_nested_braces);
    return UNITY_END();
}
