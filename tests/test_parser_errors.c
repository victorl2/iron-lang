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

/* ── Error recovery tests ────────────────────────────────────────────────── */

/* A single syntax error produces exactly 1 diagnostic */
void test_single_parse_error(void) {
    /* "func { }" — missing function name */
    parse("func { }");
    TEST_ASSERT_EQUAL_INT(1, diags.error_count);
}

/* Three independent syntax errors produce exactly 3 diagnostics */
void test_three_independent_parse_errors(void) {
    /* Three independent errors, each in a separate declaration,
     * with valid code between them:
     *   1. func { }       — missing function name  (error 1)
     *   2. val x = 10     — valid
     *   3. func foo( {    — missing ) before {     (error 2 — the open paren)
     *      }
     *   4. val y = 20     — valid
     *   5. object { }     — missing object name    (error 3)
     */
    const char *src =
        "func { }\n"
        "val x = 10\n"
        "func foo( {\n"
        "}\n"
        "val y = 20\n"
        "object { }\n";
    Iron_Node *prog = parse(src);
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL_INT(3, diags.error_count);

    /* Verify the valid declarations were still parsed */
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_NOT_NULL(pr->decls);
    TEST_ASSERT_GREATER_THAN(0, pr->decl_count);
}

/* Unexpected token at top level produces IRON_ERR_UNEXPECTED_TOKEN (101) */
void test_error_code_unexpected_token(void) {
    /* A stray integer at the top level */
    parse("42");
    TEST_ASSERT_EQUAL_INT(1, diags.error_count);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_UNEXPECTED_TOKEN, diags.items[0].code);
}

/* Missing } produces IRON_ERR_EXPECTED_RBRACE (103) */
void test_error_code_expected_rbrace(void) {
    /* func without closing brace */
    parse("func foo() {\n  val x = 1\n");
    TEST_ASSERT_GREATER_OR_EQUAL(1, diags.error_count);
    /* Find the RBRACE error */
    bool found = false;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_EXPECTED_RBRACE) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* Missing ) produces IRON_ERR_EXPECTED_RPAREN (104) */
void test_error_code_expected_rparen(void) {
    /* func with missing closing paren */
    parse("func bar(x: Int {\n}\n");
    TEST_ASSERT_GREATER_OR_EQUAL(1, diags.error_count);
    bool found = false;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_EXPECTED_RPAREN) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found);
}

/* After first error, the next valid declaration still parses */
void test_error_recovery_continues_parsing(void) {
    /* Error in first decl, valid second decl */
    const char *src =
        "func { }\n"        /* error — missing name */
        "val answer = 42\n";
    Iron_Node *prog = parse(src);
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL_INT(1, diags.error_count);

    Iron_Program *pr = (Iron_Program *)prog;
    /* Should have at least 2 decls: the error node and val answer */
    TEST_ASSERT_GREATER_OR_EQUAL(2, pr->decl_count);

    /* The last one (or second) should be a VAL_DECL */
    bool found_val = false;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_VAL_DECL) {
            Iron_ValDecl *v = (Iron_ValDecl *)pr->decls[i];
            if (strcmp(v->name, "answer") == 0) {
                found_val = true;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_val, "val answer was not parsed after error recovery");
}

/* A parse error produces an ErrorNode in the AST, not NULL */
void test_error_node_in_ast(void) {
    /* func without name: parser emits error and returns ErrorNode */
    const char *src = "func { }";
    Iron_Node *prog = parse(src);
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_PROGRAM, prog->kind);

    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_GREATER_THAN(0, pr->decl_count);

    /* At least one decl should be an ErrorNode */
    bool found_error = false;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_ERROR) {
            found_error = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_error, "Expected IRON_NODE_ERROR in AST after parse error");
}

/* Diagnostic span.line matches the line containing the error */
void test_error_span_points_to_correct_line(void) {
    /* Line 1: valid func
     * Line 3: error (func without name)
     */
    const char *src =
        "func ok() {\n"
        "}\n"
        "func { }\n";
    parse(src);
    TEST_ASSERT_EQUAL_INT(1, diags.error_count);
    /* The error is on line 3 */
    TEST_ASSERT_EQUAL_UINT(3, diags.items[0].span.line);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_parse_error);
    RUN_TEST(test_three_independent_parse_errors);
    RUN_TEST(test_error_code_unexpected_token);
    RUN_TEST(test_error_code_expected_rbrace);
    RUN_TEST(test_error_code_expected_rparen);
    RUN_TEST(test_error_recovery_continues_parsing);
    RUN_TEST(test_error_node_in_ast);
    RUN_TEST(test_error_span_points_to_correct_line);
    return UNITY_END();
}
