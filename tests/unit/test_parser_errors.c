#include "unity.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/types.h"
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

/* ── Phase 59 01d — Parser safeguard (no hang on malformed val decl) ──── */

/* Regression test for the 01c hang: `val ( something_weird }` must NOT cause
 * unbounded parser advance. Must emit at least one diagnostic AND return
 * within a bounded number of parser advances. */
void test_parser_no_hang_on_malformed_val_decl(void) {
    /* The exact 01c repro shape: stray `(` after `val` inside a block, with
     * no closing `)`. Before the safeguard this hung for 20+ seconds and
     * allocated 3.6 GB RSS. After the safeguard it must terminate in O(N)
     * tokens with at least one diagnostic. */
    const char *src = "func main() { val ( something_weird }\n";
    Iron_Node *prog = parse(src);
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
}

/* Guard against top-level `val (` hangs too. */
void test_parser_no_hang_on_unsupported_toplevel(void) {
    /* Stray `(` at top level. */
    const char *src = "( invalid\n";
    Iron_Node *prog = parse(src);
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
}

/* ── Phase 59 01d — Iron_Type tuple construction ──────────────────────── */

void test_iron_type_make_tuple_2_elem(void) {
    iron_types_init(&arena);
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_ty = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *elems[2] = { int_ty, str_ty };
    Iron_Type *tup = iron_type_make_tuple(&arena, elems, 2);
    TEST_ASSERT_NOT_NULL(tup);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_TUPLE, tup->kind);
    TEST_ASSERT_EQUAL_INT(2, tup->tuple.elem_count);
    TEST_ASSERT_NOT_NULL(tup->tuple.mangled_name);
    /* Must start with Iron_Tuple_ */
    TEST_ASSERT_EQUAL_STRING_LEN("Iron_Tuple_", tup->tuple.mangled_name, 11);
}

void test_iron_type_equals_tuple_same(void) {
    iron_types_init(&arena);
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *elems1[2] = { int_ty, int_ty };
    Iron_Type *elems2[2] = { int_ty, int_ty };
    Iron_Type *t1 = iron_type_make_tuple(&arena, elems1, 2);
    Iron_Type *t2 = iron_type_make_tuple(&arena, elems2, 2);
    TEST_ASSERT_TRUE(iron_type_equals(t1, t2));
}

void test_iron_type_equals_tuple_mismatched_arity(void) {
    iron_types_init(&arena);
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *e2[2] = { int_ty, int_ty };
    Iron_Type *e3[3] = { int_ty, int_ty, int_ty };
    Iron_Type *t2 = iron_type_make_tuple(&arena, e2, 2);
    Iron_Type *t3 = iron_type_make_tuple(&arena, e3, 3);
    TEST_ASSERT_FALSE(iron_type_equals(t2, t3));
}

void test_iron_type_equals_tuple_mismatched_elem(void) {
    iron_types_init(&arena);
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_ty = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *e_is[2] = { int_ty, str_ty };
    Iron_Type *e_ii[2] = { int_ty, int_ty };
    Iron_Type *t_is = iron_type_make_tuple(&arena, e_is, 2);
    Iron_Type *t_ii = iron_type_make_tuple(&arena, e_ii, 2);
    TEST_ASSERT_FALSE(iron_type_equals(t_is, t_ii));
}

void test_iron_type_to_string_tuple(void) {
    iron_types_init(&arena);
    Iron_Type *int_ty = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *str_ty = iron_type_make_primitive(IRON_TYPE_STRING);
    Iron_Type *elems[2] = { int_ty, str_ty };
    Iron_Type *tup = iron_type_make_tuple(&arena, elems, 2);
    const char *s = iron_type_to_string(tup, &arena);
    TEST_ASSERT_NOT_NULL(s);
    /* Must contain "(" and "," */
    TEST_ASSERT_NOT_NULL(strchr(s, '('));
    TEST_ASSERT_NOT_NULL(strchr(s, ','));
    TEST_ASSERT_NOT_NULL(strstr(s, "Int"));
    TEST_ASSERT_NOT_NULL(strstr(s, "String"));
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
    /* Phase 59 01d */
    RUN_TEST(test_parser_no_hang_on_malformed_val_decl);
    RUN_TEST(test_parser_no_hang_on_unsupported_toplevel);
    RUN_TEST(test_iron_type_make_tuple_2_elem);
    RUN_TEST(test_iron_type_equals_tuple_same);
    RUN_TEST(test_iron_type_equals_tuple_mismatched_arity);
    RUN_TEST(test_iron_type_equals_tuple_mismatched_elem);
    RUN_TEST(test_iron_type_to_string_tuple);
    return UNITY_END();
}
