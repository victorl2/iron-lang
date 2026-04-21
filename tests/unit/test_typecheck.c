/* test_typecheck.c — Unity tests for the Iron type checker.
 *
 * Tests cover:
 *   - Type inference for val/var (Int, Float, Bool, String, null)
 *   - Explicit type annotation match (no error)
 *   - Explicit type annotation mismatch => E0202
 *   - val reassignment => E0203
 *   - var reassignment OK (no error)
 *   - Function return type match (no error)
 *   - Return type mismatch => E0215
 *   - Arg count mismatch => E0216
 *   - Arg type mismatch => E0217
 *   - Nullable access without check => E0204
 *   - Nullable narrowing with != null (no error)
 *   - Early-return narrowing with == null (no error)
 *   - is-narrowing inside if block (no error)
 *   - Interface completeness: missing method => E0205
 *   - ConstructExpr for type produces object (no error)
 *   - Binary expr type inference (Int + Int = Int)
 *   - Comparison expr type = Bool
 *   - No implicit numeric conversion (Int + Float) => E0222
 *   - Float literal infers Float type
 */

#include "unity.h"
#include "analyzer/resolve.h"
#include "analyzer/typecheck.h"
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

/* ── Parse + resolve + typecheck helper ──────────────────────────────────── */

static Iron_Program *parse_and_resolve(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);
    iron_typecheck(prog, global, &g_arena, &g_diags);
    return prog;
}

/* Check if a specific error code was emitted */
static bool has_error(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* Find the first val/var decl in the first function's body */
static Iron_Node *get_first_stmt(Iron_Program *prog) {
    if (!prog || prog->decl_count == 0) return NULL;
    Iron_FuncDecl *fd = (Iron_FuncDecl *)prog->decls[0];
    if (!fd || fd->kind != IRON_NODE_FUNC_DECL) return NULL;
    Iron_Block *body = (Iron_Block *)fd->body;
    if (!body || body->stmt_count == 0) return NULL;
    return body->stmts[0];
}

/* ── Test 1: val x = 42  =>  declared_type = IRON_TYPE_INT ─────────────── */

void test_infer_int_literal(void) {
    const char *src =
        "func main() {\n"
        "  val x = 42\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_ValDecl *vd = (Iron_ValDecl *)get_first_stmt(prog);
    TEST_ASSERT_NOT_NULL(vd);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_VAL_DECL, vd->kind);
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, vd->declared_type->kind);
}

/* ── Test 2: val y = 3.14  =>  declared_type = IRON_TYPE_FLOAT ─────────── */

void test_infer_float_literal(void) {
    const char *src =
        "func main() {\n"
        "  val y = 3.14\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_ValDecl *vd = (Iron_ValDecl *)get_first_stmt(prog);
    TEST_ASSERT_NOT_NULL(vd);
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_FLOAT, vd->declared_type->kind);
}

/* ── Test 3: val b = true  =>  declared_type = IRON_TYPE_BOOL ──────────── */

void test_infer_bool_literal(void) {
    const char *src =
        "func main() {\n"
        "  val b = true\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_ValDecl *vd = (Iron_ValDecl *)get_first_stmt(prog);
    TEST_ASSERT_NOT_NULL(vd);
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_BOOL, vd->declared_type->kind);
}

/* ── Test 4: val x: Int = 42  =>  no error, explicit annotation matches ─── */

void test_explicit_annotation_match(void) {
    const char *src =
        "func main() {\n"
        "  val x: Int = 42\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    (void)prog;
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 5: val x: Float = 42  =>  E0202 (Int to Float mismatch) ────────── */

void test_explicit_annotation_mismatch(void) {
    const char *src =
        "func main() {\n"
        "  val x: Float = 42\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Test 6: val x = 1; x = 2  =>  E0203 (val reassignment) ─────────────── */

void test_val_reassignment_error(void) {
    const char *src =
        "func main() {\n"
        "  val x = 1\n"
        "  x = 2\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_VAL_REASSIGN));
}

/* ── Test 7: var x = 1; x = 2  =>  no error ─────────────────────────────── */

void test_var_reassignment_ok(void) {
    const char *src =
        "func main() {\n"
        "  var x = 1\n"
        "  x = 2\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_VAL_REASSIGN));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Test 8: func with return type match => no error ─────────────────────── */

void test_return_type_match(void) {
    const char *src =
        "func foo(a: Int) -> Int {\n"
        "  return a\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 9: func returns Bool when Int expected => E0215 ───────────────── */

void test_return_type_mismatch(void) {
    const char *src =
        "func foo() -> Int {\n"
        "  return true\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_RETURN_TYPE));
}

/* ── Test 10: func foo(a: Int) {}; foo(1, 2) => E0216 (arg count) ─────────── */

void test_arg_count_mismatch(void) {
    const char *src =
        "func foo(a: Int) {}\n"
        "func main() {\n"
        "  foo(1, 2)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_ARG_COUNT));
}

/* ── Test 11: func foo(a: Int) {}; foo(true) => E0217 (arg type) ──────────── */

void test_arg_type_mismatch(void) {
    const char *src =
        "func foo(a: Int) {}\n"
        "func main() {\n"
        "  foo(true)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_ARG_TYPE));
}

/* ── Test 12: nullable access without check => E0204 ─────────────────────── */

void test_nullable_access_no_check(void) {
    const char *src =
        "func foo(x: Int?) -> Int {\n"
        "  return x\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_NULLABLE_ACCESS));
}

/* ── Test 13: nullable narrowing with != null (no error) ─────────────────── */

void test_nullable_narrowing_neq_null(void) {
    const char *src =
        "func foo(x: Int?) -> Int {\n"
        "  if x != null {\n"
        "    return x\n"
        "  }\n"
        "  return 0\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_NULLABLE_ACCESS));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_RETURN_TYPE));
}

/* ── Test 14: early-return narrowing with == null (no error) ─────────────── */

void test_nullable_early_return_narrowing(void) {
    const char *src =
        "func foo(x: Int?) -> Int {\n"
        "  if x == null {\n"
        "    return 0\n"
        "  }\n"
        "  return x\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_NULLABLE_ACCESS));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_RETURN_TYPE));
}

/* ── Test 15: interface completeness — missing method => E0205 ───────────── */

void test_interface_missing_method(void) {
    const char *src =
        "interface Drawable {\n"
        "  func draw() -> void\n"
        "}\n"
        "object Sprite impl Drawable {\n"
        "  var x: Int\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_MISSING_IFACE_METHOD));
}

/* ── Test 16: binary expr type — 1 + 2 has resolved_type IRON_TYPE_INT ───── */

void test_binary_int_type(void) {
    const char *src =
        "func main() {\n"
        "  val z = 1 + 2\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_ValDecl *vd = (Iron_ValDecl *)get_first_stmt(prog);
    TEST_ASSERT_NOT_NULL(vd);
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, vd->declared_type->kind);

    /* Also check the binary expr's resolved_type */
    Iron_BinaryExpr *be = (Iron_BinaryExpr *)vd->init;
    TEST_ASSERT_NOT_NULL(be);
    TEST_ASSERT_NOT_NULL(be->resolved_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, be->resolved_type->kind);
}

/* ── Test 17: comparison expr 1 < 2 has resolved_type IRON_TYPE_BOOL ────── */

void test_comparison_expr_type(void) {
    const char *src =
        "func main() {\n"
        "  val b = 1 < 2\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_ValDecl *vd = (Iron_ValDecl *)get_first_stmt(prog);
    TEST_ASSERT_NOT_NULL(vd);
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_BOOL, vd->declared_type->kind);
}

/* ── Test 18: 1 + 1.0 => E0222 (no implicit numeric conversion) ─────────── */

void test_no_implicit_numeric_conversion(void) {
    const char *src =
        "func main() {\n"
        "  val z = 1 + 1.0\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_NUMERIC_CONVERSION));
}

/* ── Test 19: ConstructExpr for a type produces object ───────────────────── */

void test_construct_expr_type(void) {
    const char *src =
        "object Point {\n"
        "  var x: Int\n"
        "  var y: Int\n"
        "}\n"
        "func main() {\n"
        "  val p = Point(1, 2)\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Second decl is func main; get first stmt */
    Iron_FuncDecl *fd = (Iron_FuncDecl *)prog->decls[1];
    Iron_Block *body = (Iron_Block *)fd->body;
    Iron_ValDecl *vd = (Iron_ValDecl *)body->stmts[0];
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_OBJECT, vd->declared_type->kind);
}

/* ── Test 20: float literal 1.0 + 2.0 infers IRON_TYPE_FLOAT ────────────── */

void test_binary_float_type(void) {
    const char *src =
        "func main() {\n"
        "  val z = 1.0 + 2.0\n"
        "}\n";
    Iron_Program *prog = parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_ValDecl *vd = (Iron_ValDecl *)get_first_stmt(prog);
    TEST_ASSERT_NOT_NULL(vd);
    TEST_ASSERT_NOT_NULL(vd->declared_type);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_FLOAT, vd->declared_type->kind);
}

/* ── Test 21: is-narrowing — e is Player narrows type in if block ─────────── */

void test_is_narrowing(void) {
    /* A function with an is-check: no type errors inside block */
    const char *src =
        "object Entity {\n"
        "  var id: Int\n"
        "}\n"
        "object Player extends Entity {\n"
        "  var name: Int\n"
        "}\n"
        "func check(e: Entity) {\n"
        "  if e is Player {\n"
        "    val n = 1\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Test 22: interface completeness — all methods present => no error ────── */

void test_interface_complete_no_error(void) {
    const char *src =
        "interface Drawable {\n"
        "  func draw() -> void\n"
        "}\n"
        "object Sprite impl Drawable {\n"
        "  var x: Int\n"
        "}\n"
        "func Sprite.draw() {\n"
        "  val n = 1\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_MISSING_IFACE_METHOD));
}

/* ── Test 23: val x: Int32 = 42 => no error (literal narrowing allowed) ─────── */

void test_int32_literal_narrowing_no_error(void) {
    const char *src =
        "func main() {\n"
        "  val x: Int32 = 42\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 24: val n: Int = 10; val x: Int32 = n => E0202 (variable narrowing) ─ */

void test_int32_variable_narrowing_error(void) {
    const char *src =
        "func main() {\n"
        "  val n: Int = 10\n"
        "  val x: Int32 = n\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Test 25: val x: Int32 = 42; val y: Int = x => no error (widening) ───────── */

void test_int32_widening_no_error(void) {
    const char *src =
        "func main() {\n"
        "  val x: Int32 = 42\n"
        "  val y: Int = x\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Test 26: val n: Int = 10; val x: Int32 = Int32(n) => no error (cast) ────── */

void test_int32_explicit_cast_no_error(void) {
    const char *src =
        "func main() {\n"
        "  val n: Int = 10\n"
        "  val x: Int32 = Int32(n)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── ADT type checker tests ───────────────────────────────────────────────── */

/* Test 27: Enum construct produces an IRON_TYPE_ENUM-typed expression */
void test_tc_adt_enum_construct_type(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 28: Arity mismatch on enum construct produces IRON_ERR_PATTERN_ARITY (225) */
void test_tc_adt_enum_construct_arity_error(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0, 2.0)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(225));
}

/* Test 29: Non-exhaustive match on ADT enum produces IRON_ERR_NONEXHAUSTIVE_MATCH (224) */
void test_tc_adt_exhaustive_error(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "  Rect(Float, Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0)\n"
        "  match s {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = r\n"
        "    }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(224));
}

/* Test 30: else arm satisfies exhaustiveness — no error */
void test_tc_adt_exhaustive_with_else(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "  Rect(Float, Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0)\n"
        "  match s {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = r\n"
        "    }\n"
        "    else -> {\n"
        "      val y = 0\n"
        "    }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 31: Duplicate variant arm produces IRON_ERR_UNREACHABLE_ARM (226) */
void test_tc_adt_unreachable_arm(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "  Rect(Float, Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0)\n"
        "  match s {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = r\n"
        "    }\n"
        "    Shape.Circle(z) -> {\n"
        "      val y = z\n"
        "    }\n"
        "    Shape.Rect(w, h) -> {\n"
        "      val a = w\n"
        "    }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(226));
}

/* Test 32: All variants explicitly covered — no error */
void test_tc_adt_exhaustive_all_variants(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "  Rect(Float, Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0)\n"
        "  match s {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = r\n"
        "    }\n"
        "    Shape.Rect(w, h) -> {\n"
        "      val y = w\n"
        "    }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 33: Wildcard _ in pattern does not introduce binding — no error */
void test_tc_adt_wildcard_in_pattern(void) {
    const char *src =
        "enum Shape {\n"
        "  Rect(Float, Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Rect(1.0, 2.0)\n"
        "  match s {\n"
        "    Shape.Rect(_, h) -> {\n"
        "      val x = h\n"
        "    }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 34: Integer match with else — no exhaustiveness error (regression) */
void test_tc_adt_integer_match_unchanged(void) {
    const char *src =
        "func main() {\n"
        "  val x = 42\n"
        "  match x {\n"
        "    1 -> { val a = 1 }\n"
        "    2 -> { val b = 2 }\n"
        "    else -> { val c = 0 }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Semantic analysis gap tests ──────────────────────────────────────────── */

/* Test: non-exhaustive match on enum (missing variant, plain enum) */
void test_nonexhaustive_match_enum(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red -> { }\n"
        "    Green -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_NONEXHAUSTIVE_MATCH));
}

/* Test: exhaustive match covering all variants => no error */
void test_exhaustive_match_all_variants(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red -> { }\n"
        "    Green -> { }\n"
        "    Blue -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_NONEXHAUSTIVE_MATCH));
}

/* Test: match with else clause => no error even with missing variants */
void test_exhaustive_match_with_else(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red -> { }\n"
        "    else -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test: non-exhaustive match on non-enum type */
void test_nonexhaustive_match_non_enum(void) {
    const char *src =
        "func main() {\n"
        "  val x: Int = 5\n"
        "  match x {\n"
        "    1 -> { }\n"
        "    2 -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_NONEXHAUSTIVE_MATCH));
}

/* Test: match on non-enum with else => no error */
void test_match_non_enum_with_else(void) {
    const char *src =
        "func main() {\n"
        "  val x: Int = 5\n"
        "  match x {\n"
        "    1 -> { }\n"
        "    else -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test: duplicate match arm */
void test_duplicate_match_arm(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red -> { }\n"
        "    Red -> { }\n"
        "    Green -> { }\n"
        "    Blue -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_DUPLICATE_MATCH_ARM));
}

/* ── Test 32b: unique match arms => no IRON_ERR_DUPLICATE_MATCH_ARM ──────── */

void test_unique_match_arms_no_duplicate_error(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n"
        "func main() {\n"
        "  val c = Red\n"
        "  match c {\n"
        "    Red -> { }\n"
        "    Green -> { }\n"
        "    Blue -> { }\n"
        "  }\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_DUPLICATE_MATCH_ARM));
}

/* ── Test 33: cast from non-numeric/non-bool source => E0310 ─────────────── */

void test_cast_invalid_source(void) {
    const char *src =
        "func main() {\n"
        "  val s: String = \"hello\"\n"
        "  val n = Int(s)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INVALID_CAST));
}

/* ── Test 34: Bool->Int cast is allowed (no error) ──────────────────────── */

void test_cast_bool_to_int_ok(void) {
    const char *src =
        "func main() {\n"
        "  val b = true\n"
        "  val n = Int(b)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INVALID_CAST));
}

/* ── Test 35: Int->Bool cast is rejected => E0310 ───────────────────────── */

void test_cast_int_to_bool_error(void) {
    const char *src =
        "func main() {\n"
        "  val n = 42\n"
        "  val b = Bool(n)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INVALID_CAST));
}

/* ── Test 36: wider-to-narrower integer cast warns => W0601 ─────────────── */

void test_cast_narrowing_warning(void) {
    const char *src =
        "func main() {\n"
        "  val n: Int = 1000\n"
        "  val x = Int8(n)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_WARN_NARROWING_CAST));
}

/* ── Test 37: narrower-to-wider integer cast is silent ──────────────────── */

void test_cast_widening_no_warning(void) {
    const char *src =
        "func main() {\n"
        "  val n: Int32 = 42\n"
        "  val x = Int(n)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_NARROWING_CAST));
}

/* ── Test 38: constant literal overflows target => E0311 ────────────────── */

void test_cast_overflow_constant(void) {
    const char *src =
        "func main() {\n"
        "  val x = Int8(300)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_CAST_OVERFLOW));
}

/* ── Test 39: constant literal fits target => no warning or error ────────── */

void test_cast_constant_fits_no_warning(void) {
    const char *src =
        "func main() {\n"
        "  val x = Int8(42)\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_NARROWING_CAST));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_CAST_OVERFLOW));
}

/* ── Test 40: string interpolation with primitive => no W0602 ────────────── */

void test_interp_primitive_no_warning(void) {
    const char *src =
        "func main() {\n"
        "  val n = 42\n"
        "  val s = \"value is {n}\"\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_NOT_STRINGABLE));
}

/* ── Test 41: string interpolation with Bool => no W0602 ────────────────── */

void test_interp_bool_no_warning(void) {
    const char *src =
        "func main() {\n"
        "  val b = true\n"
        "  val s = \"value is {b}\"\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_NOT_STRINGABLE));
}

/* ── Test 42: non-stringifiable object in interpolation => W0602 ─────────── */

void test_interp_not_stringable(void) {
    const char *src =
        "object Foo {\n"
        "  val x: Int\n"
        "}\n"
        "func main() {\n"
        "  val f = Foo(1)\n"
        "  val s = \"value is {f}\"\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_WARN_NOT_STRINGABLE));
}

/* ── Test 43: object with to_string() in interpolation => no W0602 ───────── */

void test_interp_object_with_to_string_ok(void) {
    const char *src =
        "object Bar {\n"
        "  val x: Int\n"
        "}\n"
        "func Bar.to_string() -> String {\n"
        "  return \"Bar\"\n"
        "}\n"
        "func main() {\n"
        "  val b = Bar(1)\n"
        "  val s = \"value is {b}\"\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_NOT_STRINGABLE));
}

/* ── Test 44: compound assign on narrow Int8, non-constant RHS => W0603 ──── */

void test_compound_narrow_overflow_warning(void) {
    const char *src =
        "func main() {\n"
        "  var x: Int8 = 1\n"
        "  var y: Int = 100\n"
        "  x += y\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_WARN_POSSIBLE_OVERFLOW));
}

/* ── Test 45: compound assign on narrow Int8, constant that fits => no W0603 */

void test_compound_narrow_constant_fits_no_warning(void) {
    const char *src =
        "func main() {\n"
        "  var x: Int8 = 1\n"
        "  x += 5\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_POSSIBLE_OVERFLOW));
}

/* ── Test 46: compound assign on narrow Int8, constant overflows => W0603 ── */

void test_compound_narrow_constant_overflows_warning(void) {
    const char *src =
        "func main() {\n"
        "  var x: Int8 = 1\n"
        "  x += 200\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_WARN_POSSIBLE_OVERFLOW));
}

/* ── Test 47: compound assign on full-width Int => no W0603 ────────────────── */

void test_compound_full_width_no_warning(void) {
    const char *src =
        "func main() {\n"
        "  var x: Int = 1\n"
        "  var y: Int = 100\n"
        "  x += y\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_WARN_POSSIBLE_OVERFLOW));
}

/* ── Test 48: compound -= on narrow UInt8 => W0603 ─────────────────────────── */

void test_compound_subtract_narrow_warning(void) {
    const char *src =
        "func main() {\n"
        "  var x: UInt8 = 10\n"
        "  var y: Int = 5\n"
        "  x -= y\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_WARN_POSSIBLE_OVERFLOW));
}

/* ── Test 49: arr[5] on [Int; 3] => E0312 index out of bounds ────────────── */

void test_index_out_of_bounds_high(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val x = arr[5]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INDEX_OUT_OF_BOUNDS));
}

/* ── Test 50: arr[2] on [Int; 3] => no error (valid last index) ─────────── */

void test_index_valid_last(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val x = arr[2]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INDEX_OUT_OF_BOUNDS));
}

/* ── Test 51: arr[-1] on [Int; 3] => E0312 index out of bounds ──────────── */

void test_index_negative(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val x = arr[-1]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INDEX_OUT_OF_BOUNDS));
}

/* ── Test 52: arr["hello"] => E0222 type mismatch (non-integer index) ───── */

void test_index_non_integer_type(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val x = arr[\"hello\"]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Test 53: arr[0] on [Int; 3] => no error (valid first index) ────────── */

void test_index_valid_zero(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val x = arr[0]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INDEX_OUT_OF_BOUNDS));
}

/* ── Test 54: arr[i] (non-constant) => no bounds error (skip) ───────────── */

void test_index_non_constant_skip(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  var i = 1\n"
        "  val x = arr[i]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INDEX_OUT_OF_BOUNDS));
}

/* ── Test 55: arr[0:2] on [Int; 3] => no error (valid slice) ───────────── */

void test_slice_valid(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val s = arr[0..2]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INVALID_SLICE_BOUNDS));
}

/* ── Test 56: arr[2:1] on [Int; 3] => IRON_ERR_INVALID_SLICE_BOUNDS ───── */

void test_slice_start_greater_than_end(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val s = arr[2..1]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INVALID_SLICE_BOUNDS));
}

/* ── Test 57: arr[0:4] on [Int; 3] => IRON_ERR_INVALID_SLICE_BOUNDS ───── */

void test_slice_end_exceeds_size(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val s = arr[0..4]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INVALID_SLICE_BOUNDS));
}

/* ── Test 58: arr[-1:2] on [Int; 3] => IRON_ERR_INVALID_SLICE_BOUNDS ──── */

void test_slice_negative_start(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val s = arr[-1..2]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INVALID_SLICE_BOUNDS));
}

/* ── Test 59: arr["a":"b"] => IRON_ERR_TYPE_MISMATCH (non-integer) ─────── */

void test_slice_non_integer_bounds(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val s = arr[\"a\"..\"b\"]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Test 60: arr[x:y] (non-constant) => no bounds error (skip) ───────── */

void test_slice_non_constant_skip(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  var x = 0\n"
        "  var y = 2\n"
        "  val s = arr[x..y]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INVALID_SLICE_BOUNDS));
}

/* ── Test 61: arr[0:3] on [Int; 3] => no error (end == size, exclusive) ── */

void test_slice_end_equals_size_valid(void) {
    const char *src =
        "func main() {\n"
        "  val arr: [Int; 3] = [1, 2, 3]\n"
        "  val s = arr[0..3]\n"
        "}\n";
    parse_and_resolve(src);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INVALID_SLICE_BOUNDS));
}

/* ── Generic constraint checking tests ────────────────────────────────────── */

/* GEN-01 negative: constraint satisfied on function call => no error */
void test_generic_constraint_satisfied_no_error(void) {
    parse_and_resolve(
        "interface Printable {\n"
        "  func to_string() -> String\n"
        "}\n"
        "object MyObj impl Printable {\n"
        "  var value: Int\n"
        "}\n"
        "func MyObj.to_string() -> String {\n"
        "  return \"obj\"\n"
        "}\n"
        "func show[T: Printable](x: T) {\n"
        "  println(\"shown\")\n"
        "}\n"
        "func main() {\n"
        "  show(MyObj(1))\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_GENERIC_CONSTRAINT));
}

/* GEN-02 positive: constraint violated on function call => error */
void test_generic_constraint_violated_function_call(void) {
    parse_and_resolve(
        "interface Printable {\n"
        "  func to_string() -> String\n"
        "}\n"
        "func show[T: Printable](x: T) {\n"
        "  println(\"shown\")\n"
        "}\n"
        "func main() {\n"
        "  show(42)\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_GENERIC_CONSTRAINT));
}

/* GEN-03 positive: constraint violated on construction => error
 * Uses call-as-construction path: Container(42) where field type T
 * is inferred from the Int arg, which does not implement Printable. */
void test_generic_constraint_violated_construction(void) {
    parse_and_resolve(
        "interface Printable {\n"
        "  func to_string() -> String\n"
        "}\n"
        "object Container[T: Printable] {\n"
        "  var item: T\n"
        "}\n"
        "func main() {\n"
        "  val c = Container(42)\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_GENERIC_CONSTRAINT));
}

/* GEN-03 negative: constraint satisfied on construction => no error
 * Uses call-as-construction path: Container(MyObj(1)) where field type T
 * is inferred from the MyObj arg, which impl Printable. */
void test_generic_constraint_satisfied_construction(void) {
    parse_and_resolve(
        "interface Printable {\n"
        "  func to_string() -> String\n"
        "}\n"
        "object MyObj impl Printable {\n"
        "  var value: Int\n"
        "}\n"
        "func MyObj.to_string() -> String {\n"
        "  return \"obj\"\n"
        "}\n"
        "object Container[T: Printable] {\n"
        "  var item: T\n"
        "}\n"
        "func main() {\n"
        "  val c = Container(MyObj(1))\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_GENERIC_CONSTRAINT));
}

/* GEN-01: unconstrained generic produces no constraint error */
void test_generic_unconstrained_no_error(void) {
    parse_and_resolve(
        "func identity[T](x: T) -> T {\n"
        "  return x\n"
        "}\n"
        "func main() {\n"
        "  val r = identity(42)\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_GENERIC_CONSTRAINT));
}

/* GEN-04: error message names constraint and type */
void test_generic_constraint_error_message(void) {
    parse_and_resolve(
        "interface Sortable {\n"
        "  func compare() -> Int\n"
        "}\n"
        "func sort_it[T: Sortable](x: T) {\n"
        "  println(\"sorted\")\n"
        "}\n"
        "func main() {\n"
        "  sort_it(42)\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_GENERIC_CONSTRAINT));
    /* Verify the message mentions the constraint name */
    bool found_msg = false;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == IRON_ERR_GENERIC_CONSTRAINT &&
            g_diags.items[i].message &&
            strstr(g_diags.items[i].message, "Sortable") != NULL) {
            found_msg = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_msg);
}

/* ── Phase 59 01d: tuple types, literals, destructure, equality ───────── */

void test_typecheck_tuple_return_type(void) {
    parse_and_resolve(
        "func pair() -> (Int, Int) {\n"
        "  return (1, 2)\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_RETURN_TYPE));
}

void test_typecheck_tuple_destructure_arity_ok(void) {
    parse_and_resolve(
        "func pair() -> (Int, Int) {\n"
        "  return (1, 2)\n"
        "}\n"
        "func main() {\n"
        "  val (a, b) = pair()\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
}

void test_typecheck_tuple_destructure_arity_wrong(void) {
    parse_and_resolve(
        "func pair() -> (Int, Int) {\n"
        "  return (1, 2)\n"
        "}\n"
        "func main() {\n"
        "  val (a, b, c) = pair()\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

void test_typecheck_tuple_destructure_non_tuple_init(void) {
    parse_and_resolve(
        "func main() {\n"
        "  val (a, b) = 42\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

void test_typecheck_tuple_equality_ok(void) {
    parse_and_resolve(
        "func main() {\n"
        "  val a: (Int, Int) = (1, 2)\n"
        "  val b: (Int, Int) = (1, 2)\n"
        "  val eq: Bool = a == b\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
}

void test_typecheck_tuple_equality_mismatched_arity(void) {
    /* Force arity mismatch via declared types. */
    parse_and_resolve(
        "func main() {\n"
        "  val a: (Int, Int) = (1, 2)\n"
        "  val b: (Int, Int, Int) = (1, 2, 3)\n"
        "  val eq = a == b\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

void test_typecheck_tuple_equality_mismatched_element_type(void) {
    parse_and_resolve(
        "func main() {\n"
        "  val a: (Int, Int) = (1, 2)\n"
        "  val b: (Int, String) = (1, \"two\")\n"
        "  val eq = a == b\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_TYPE_MISMATCH));
}

void test_typecheck_nested_tuple(void) {
    parse_and_resolve(
        "func coords() -> (Int, (Int, Int)) {\n"
        "  return (1, (2, 3))\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
}

/* ── Phase 83-02 Task 2: property-syntax dispatch flag plumbing ─────────────
 * typecheck sets Iron_FieldAccess.is_pub_access when the resolved field
 * carries is_pub, and sets Iron_AssignStmt.is_pub_setter when the assignment
 * target is a pub var field. Writing to a pub val field (pub without var)
 * emits IRON_ERR_VAL_REASSIGN with a message naming 'pub val'. HIR consumes
 * these bits in a separate end-to-end smoke-compile test. */

/* Find the FieldAccess expression in the first println/val-init statement of
 * the function named `func_name`. Returns NULL when not found. Walks
 * down common expression nests: interpolated-string parts, val/var init. */
static Iron_FieldAccess *find_field_access_in_func(Iron_Program *prog,
                                                   const char *func_name) {
    for (int di = 0; di < prog->decl_count; di++) {
        Iron_Node *dn = prog->decls[di];
        if (dn->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)dn;
        if (!fd->name || strcmp(fd->name, func_name) != 0) continue;
        Iron_Block *body = (Iron_Block *)fd->body;
        if (!body) return NULL;
        for (int si = 0; si < body->stmt_count; si++) {
            Iron_Node *st = body->stmts[si];
            /* val/var init may directly be a FIELD_ACCESS */
            Iron_Node *init = NULL;
            if (st->kind == IRON_NODE_VAL_DECL) init = ((Iron_ValDecl *)st)->init;
            else if (st->kind == IRON_NODE_VAR_DECL) init = ((Iron_VarDecl *)st)->init;
            if (init && init->kind == IRON_NODE_FIELD_ACCESS) {
                return (Iron_FieldAccess *)init;
            }
            if (init && init->kind == IRON_NODE_INTERP_STRING) {
                Iron_InterpString *is = (Iron_InterpString *)init;
                for (int pi = 0; pi < is->part_count; pi++) {
                    if (is->parts[pi] &&
                        is->parts[pi]->kind == IRON_NODE_FIELD_ACCESS) {
                        return (Iron_FieldAccess *)is->parts[pi];
                    }
                }
            }
        }
    }
    return NULL;
}

/* Find the first IRON_NODE_ASSIGN statement in the function body. */
static Iron_AssignStmt *find_assign_in_func(Iron_Program *prog,
                                            const char *func_name) {
    for (int di = 0; di < prog->decl_count; di++) {
        Iron_Node *dn = prog->decls[di];
        if (dn->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *fd = (Iron_FuncDecl *)dn;
        if (!fd->name || strcmp(fd->name, func_name) != 0) continue;
        Iron_Block *body = (Iron_Block *)fd->body;
        if (!body) return NULL;
        for (int si = 0; si < body->stmt_count; si++) {
            if (body->stmts[si]->kind == IRON_NODE_ASSIGN) {
                return (Iron_AssignStmt *)body->stmts[si];
            }
        }
    }
    return NULL;
}

void test_pub_field_read_sets_is_pub_access(void) {
    Iron_Program *prog = parse_and_resolve(
        "object P {\n"
        "  pub val x: Int\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P(0)\n"
        "  val r: Int = p.x\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
    Iron_FieldAccess *fa = find_field_access_in_func(prog, "main");
    TEST_ASSERT_NOT_NULL(fa);
    TEST_ASSERT_EQUAL_STRING("x", fa->field);
    TEST_ASSERT_TRUE(fa->is_pub_access);
}

void test_pub_field_write_sets_is_pub_setter(void) {
    Iron_Program *prog = parse_and_resolve(
        "object Q {\n"
        "  pub var h: Int\n"
        "}\n"
        "func main() {\n"
        "  var q: Q = Q(0)\n"
        "  q.h = 5\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
    Iron_AssignStmt *as = find_assign_in_func(prog, "main");
    TEST_ASSERT_NOT_NULL(as);
    TEST_ASSERT_TRUE(as->is_pub_setter);
}

void test_non_pub_field_read_is_pub_access_false(void) {
    Iron_Program *prog = parse_and_resolve(
        "object R {\n"
        "  val unsealed: Int\n"
        "}\n"
        "func main() {\n"
        "  val r: R = R(0)\n"
        "  val u: Int = r.unsealed\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
    Iron_FieldAccess *fa = find_field_access_in_func(prog, "main");
    TEST_ASSERT_NOT_NULL(fa);
    TEST_ASSERT_EQUAL_STRING("unsealed", fa->field);
    TEST_ASSERT_FALSE(fa->is_pub_access);
}

void test_non_pub_field_write_is_pub_setter_false(void) {
    Iron_Program *prog = parse_and_resolve(
        "object S {\n"
        "  var m: Int\n"
        "}\n"
        "func main() {\n"
        "  var s: S = S(0)\n"
        "  s.m = 9\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_TYPE_MISMATCH));
    Iron_AssignStmt *as = find_assign_in_func(prog, "main");
    TEST_ASSERT_NOT_NULL(as);
    TEST_ASSERT_FALSE(as->is_pub_setter);
}

void test_pub_val_assign_rejected_with_val_reassign(void) {
    /* pub val is read-only: assigning to a pub val field through property
     * syntax must emit IRON_ERR_VAL_REASSIGN (203) with a message that
     * references pub val so the error hint is specific. */
    parse_and_resolve(
        "object V {\n"
        "  pub val locked: Int\n"
        "}\n"
        "func main() {\n"
        "  var v: V = V(0)\n"
        "  v.locked = 7\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_VAL_REASSIGN));
    bool found_msg = false;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == IRON_ERR_VAL_REASSIGN &&
            g_diags.items[i].message &&
            strstr(g_diags.items[i].message, "pub val") != NULL) {
            found_msg = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_msg,
        "expected IRON_ERR_VAL_REASSIGN diagnostic citing 'pub val'");
}

/* ── Phase 84 MUTTIER-02: readonly tier enforcement (Plan 84-02 Task 1) ──── */

void test_readonly_writes_self_emits_E0238(void) {
    /* Writing self.field inside a readonly method must emit
     * IRON_ERR_READONLY_WRITE_SELF (238). The Phase 80 MUT-03 chain walk
     * is reused: walk the FieldAccess target to its root ident and check
     * the root name is "self". */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  readonly func bump() {\n"
        "    self.a = 1\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_READONLY_WRITE_SELF));
}

void test_readonly_reads_self_is_ok(void) {
    /* Reading self.field from a readonly method is legal. No E0238,
     * no E0239, no type mismatch. */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  readonly func read() -> Int {\n"
        "    return self.a\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
}

void test_readonly_calls_readonly_is_ok(void) {
    /* MUTTIER-06 positive: readonly can call readonly on self. */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  readonly func read() -> Int {\n"
        "    return self.a\n"
        "  }\n"
        "  readonly func chain() -> Int {\n"
        "    return self.read()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
}

void test_readonly_calls_mutating_emits_E0239(void) {
    /* Calling a default-mutating method from readonly context must emit
     * IRON_ERR_READONLY_CALLS_MUTATING (239). Uses Phase 80 MUT-04 callee
     * inspection extended with caller-tier check. */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  func bump() {\n"
        "    self.a = 1\n"
        "  }\n"
        "  readonly func chain() {\n"
        "    self.bump()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
}

void test_readonly_calls_pure_is_ok(void) {
    /* MUTTIER-06 positive: readonly can call pure (pure is stronger
     * than readonly, so the transitive rule allows the call). */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  pure func get() -> Int {\n"
        "    return self.a\n"
        "  }\n"
        "  readonly func chain() -> Int {\n"
        "    return self.get()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_NON_PURE_CALL));
}

void test_plain_method_calls_mutating_is_ok(void) {
    /* MUTTIER-01 regression: a default-mutating method can call any
     * tier — no tier checks fire from a non-readonly, non-pure caller. */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  func a_set() {\n"
        "    self.a = 1\n"
        "  }\n"
        "  func b() {\n"
        "    self.a_set()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
}

void test_synth_getter_callable_from_readonly(void) {
    /* Phase 83-84 bridge: every `pub val`/`pub var` getter is retrofitted
     * is_readonly=true AND is_pure=true in iron_parse_object_decl. That
     * means a readonly method can read the property without tripping
     * E0239 (mutating-call) or E0242 (non-pure call). */
    parse_and_resolve(
        "object X {\n"
        "  pub val a: Int\n"
        "  readonly func wrap() -> Int {\n"
        "    return self.a\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_infer_int_literal);
    RUN_TEST(test_infer_float_literal);
    RUN_TEST(test_infer_bool_literal);
    RUN_TEST(test_explicit_annotation_match);
    RUN_TEST(test_explicit_annotation_mismatch);
    RUN_TEST(test_val_reassignment_error);
    RUN_TEST(test_var_reassignment_ok);
    RUN_TEST(test_return_type_match);
    RUN_TEST(test_return_type_mismatch);
    RUN_TEST(test_arg_count_mismatch);
    RUN_TEST(test_arg_type_mismatch);
    RUN_TEST(test_nullable_access_no_check);
    RUN_TEST(test_nullable_narrowing_neq_null);
    RUN_TEST(test_nullable_early_return_narrowing);
    RUN_TEST(test_interface_missing_method);
    RUN_TEST(test_binary_int_type);
    RUN_TEST(test_comparison_expr_type);
    RUN_TEST(test_no_implicit_numeric_conversion);
    RUN_TEST(test_construct_expr_type);
    RUN_TEST(test_binary_float_type);
    RUN_TEST(test_is_narrowing);
    RUN_TEST(test_interface_complete_no_error);
    RUN_TEST(test_int32_literal_narrowing_no_error);
    RUN_TEST(test_int32_variable_narrowing_error);
    RUN_TEST(test_int32_widening_no_error);
    RUN_TEST(test_int32_explicit_cast_no_error);
    RUN_TEST(test_tc_adt_enum_construct_type);
    RUN_TEST(test_tc_adt_enum_construct_arity_error);
    RUN_TEST(test_tc_adt_exhaustive_error);
    RUN_TEST(test_tc_adt_exhaustive_with_else);
    RUN_TEST(test_tc_adt_unreachable_arm);
    RUN_TEST(test_tc_adt_exhaustive_all_variants);
    RUN_TEST(test_tc_adt_wildcard_in_pattern);
    RUN_TEST(test_tc_adt_integer_match_unchanged);
    RUN_TEST(test_nonexhaustive_match_enum);
    RUN_TEST(test_exhaustive_match_all_variants);
    RUN_TEST(test_exhaustive_match_with_else);
    RUN_TEST(test_nonexhaustive_match_non_enum);
    RUN_TEST(test_match_non_enum_with_else);
    RUN_TEST(test_duplicate_match_arm);
    RUN_TEST(test_unique_match_arms_no_duplicate_error);
    RUN_TEST(test_cast_invalid_source);
    RUN_TEST(test_cast_bool_to_int_ok);
    RUN_TEST(test_cast_int_to_bool_error);
    RUN_TEST(test_cast_narrowing_warning);
    RUN_TEST(test_cast_widening_no_warning);
    RUN_TEST(test_cast_overflow_constant);
    RUN_TEST(test_cast_constant_fits_no_warning);
    RUN_TEST(test_interp_primitive_no_warning);
    RUN_TEST(test_interp_bool_no_warning);
    RUN_TEST(test_interp_not_stringable);
    RUN_TEST(test_interp_object_with_to_string_ok);
    RUN_TEST(test_compound_narrow_overflow_warning);
    RUN_TEST(test_compound_narrow_constant_fits_no_warning);
    RUN_TEST(test_compound_narrow_constant_overflows_warning);
    RUN_TEST(test_compound_full_width_no_warning);
    RUN_TEST(test_compound_subtract_narrow_warning);
    RUN_TEST(test_index_out_of_bounds_high);
    RUN_TEST(test_index_valid_last);
    RUN_TEST(test_index_negative);
    RUN_TEST(test_index_non_integer_type);
    RUN_TEST(test_index_valid_zero);
    RUN_TEST(test_index_non_constant_skip);
    RUN_TEST(test_slice_valid);
    RUN_TEST(test_slice_start_greater_than_end);
    RUN_TEST(test_slice_end_exceeds_size);
    RUN_TEST(test_slice_negative_start);
    RUN_TEST(test_slice_non_integer_bounds);
    RUN_TEST(test_slice_non_constant_skip);
    RUN_TEST(test_slice_end_equals_size_valid);
    RUN_TEST(test_generic_constraint_satisfied_no_error);
    RUN_TEST(test_generic_constraint_violated_function_call);
    RUN_TEST(test_generic_constraint_violated_construction);
    RUN_TEST(test_generic_constraint_satisfied_construction);
    RUN_TEST(test_generic_unconstrained_no_error);
    RUN_TEST(test_generic_constraint_error_message);

    /* Phase 59 01d: tuple support */
    RUN_TEST(test_typecheck_tuple_return_type);
    RUN_TEST(test_typecheck_tuple_destructure_arity_ok);
    RUN_TEST(test_typecheck_tuple_destructure_arity_wrong);
    RUN_TEST(test_typecheck_tuple_destructure_non_tuple_init);
    RUN_TEST(test_typecheck_tuple_equality_ok);
    RUN_TEST(test_typecheck_tuple_equality_mismatched_arity);
    RUN_TEST(test_typecheck_tuple_equality_mismatched_element_type);
    RUN_TEST(test_typecheck_nested_tuple);

    /* Phase 83-02 Task 2: pub-field dispatch flag plumbing. */
    RUN_TEST(test_pub_field_read_sets_is_pub_access);
    RUN_TEST(test_pub_field_write_sets_is_pub_setter);
    RUN_TEST(test_non_pub_field_read_is_pub_access_false);
    RUN_TEST(test_non_pub_field_write_is_pub_setter_false);
    RUN_TEST(test_pub_val_assign_rejected_with_val_reassign);

    /* Phase 84 MUTTIER-02 Task 1: readonly tier enforcement. */
    RUN_TEST(test_readonly_writes_self_emits_E0238);
    RUN_TEST(test_readonly_reads_self_is_ok);
    RUN_TEST(test_readonly_calls_readonly_is_ok);
    RUN_TEST(test_readonly_calls_mutating_emits_E0239);
    RUN_TEST(test_readonly_calls_pure_is_ok);
    RUN_TEST(test_plain_method_calls_mutating_is_ok);
    RUN_TEST(test_synth_getter_callable_from_readonly);

    return UNITY_END();
}
