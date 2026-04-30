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
#include "vendor/stb_ds.h"

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

/* parse_and_resolve_no_strict: parse with v3_strict_mode=false for tests that
 * exercise typecheck / resolve behavior on objects that lack init constructors.
 * The E0264 init requirement is verified by gate-specific tests; these tests
 * focus on type resolution and construction semantics. */
static Iron_Program *parse_and_resolve_no_strict(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &g_arena, &g_diags);
    p.v3_strict_mode = false;
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

/* Check if any diagnostic message contains the given substring. Used by
 * Phase 86 PATCH tests to lock ASCII-only message substrings without
 * coupling to the full diagnostic wording. */
static bool has_error_msg_substring(const char *needle) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].message &&
            strstr(g_diags.items[i].message, needle) != NULL) {
            return true;
        }
    }
    return false;
}

/* Parse + resolve WITHOUT running the typechecker. Phase 86 Task 1 RED
 * tests reach into the per-program patch registry directly, which is
 * built on top of resolve's global scope; the typechecker would add
 * dispatch errors we do not want to assert on at Task 1. Side effect:
 * stashes the resolved global scope in g_global_scope_for_patch so
 * callers can pass it to iron_type_patch_registry_build. */
static Iron_Scope *g_global_scope_for_patch = NULL;

static Iron_Program *parse_and_resolve_only(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron",
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    g_global_scope_for_patch = iron_resolve(prog, &g_arena, &g_diags);
    return prog;
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
    Iron_Program *prog = parse_and_resolve_no_strict(src);
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

/* ── Phase 84 MUTTIER-03: pure tier enforcement (Plan 84-02 Task 2) ──────── */

void test_pure_writes_self_emits_E0244(void) {
    /* Pure method writing self.field must emit IRON_ERR_PURE_WRITE_SELF
     * (244), not E0238 — pure gets a distinct error code for clearer
     * diagnostic messaging. Method name is `bump` (not `mut`, which is a
     * reserved token for Phase 80 receiver-mut). */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  pure func bump() {\n"
        "    self.a = 1\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_WRITE_SELF));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_WRITE_SELF));
}

void test_pure_calls_println_emits_E0240(void) {
    /* Pure method calling println is I/O; reject with IRON_ERR_PURE_IO (240). */
    parse_and_resolve(
        "object X {\n"
        "  pure func log() {\n"
        "    println(\"hi\")\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_IO));
}

void test_pure_calls_print_emits_E0240(void) {
    /* print is also on the pure I/O blocklist. */
    parse_and_resolve(
        "object X {\n"
        "  pure func log() {\n"
        "    print(\"hi\")\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_IO));
}

void test_pure_heap_alloc_is_ok(void) {
    /* 84-CONTEXT.md locks heap-alloc-allowed for pure tier. A pure method
     * returning an array literal does not trip any tier error. */
    parse_and_resolve(
        "object X {\n"
        "  pure func mk() -> [Int] {\n"
        "    return [1, 2, 3]\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_IO));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_MUTABLE_GLOBAL));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_NON_PURE_CALL));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_PARAM_WRITE));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_WRITE_SELF));
}

void test_pure_writes_param_emits_E0243(void) {
    /* Pure method writing to a param (var n) must emit
     * IRON_ERR_PURE_PARAM_WRITE (243). Uses `var n: Int` param and
     * re-assignment `n = 5` — avoids the INDEX path entirely so the
     * check lands in the ident-write branch of IRON_NODE_ASSIGN. */
    parse_and_resolve(
        "object X {\n"
        "  pure func tweak(var n: Int) -> Int {\n"
        "    n = 5\n"
        "    return n\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_PARAM_WRITE));
}

void test_pure_calls_non_pure_method_emits_E0242(void) {
    /* Pure cannot call readonly (only pure). E0242 fires. */
    parse_and_resolve(
        "object X {\n"
        "  readonly func ro() -> Int {\n"
        "    return 1\n"
        "  }\n"
        "  pure func p() -> Int {\n"
        "    return self.ro()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_NON_PURE_CALL));
}

void test_pure_calls_pure_method_is_ok(void) {
    /* MUTTIER-06 positive: pure -> pure is legal. */
    parse_and_resolve(
        "object X {\n"
        "  pure func p1() -> Int {\n"
        "    return 1\n"
        "  }\n"
        "  pure func p2() -> Int {\n"
        "    return self.p1()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_NON_PURE_CALL));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_IO));
}

void test_pure_reads_mutable_global_emits_E0241(void) {
    /* Reading a top-level `var` from a pure method is a mutable-global
     * access; emit IRON_ERR_PURE_MUTABLE_GLOBAL (241). */
    parse_and_resolve(
        "var counter: Int = 0\n"
        "object X {\n"
        "  pure func get() -> Int {\n"
        "    return counter\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_MUTABLE_GLOBAL));
}

void test_pure_writes_mutable_global_emits_E0241(void) {
    /* Writing to a top-level `var` from a pure method is also a
     * mutable-global access; emit IRON_ERR_PURE_MUTABLE_GLOBAL. */
    parse_and_resolve(
        "var counter: Int = 0\n"
        "object X {\n"
        "  pure func set() {\n"
        "    counter = 5\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_MUTABLE_GLOBAL));
}

void test_pure_reads_val_global_is_ok(void) {
    /* Top-level `val` is immutable; pure can read it without tripping
     * E0241. */
    parse_and_resolve(
        "val PI: Float = 3.14\n"
        "object X {\n"
        "  pure func c() -> Float {\n"
        "    return PI\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_MUTABLE_GLOBAL));
}

void test_mutating_calls_on_val_binding_still_emits_E0235(void) {
    /* MUTTIER-04 regression: Phase 80 E0235 (val-bound receiver +
     * mutating method) still fires after the Plan 84-02 additions. */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  func bump() {\n"
        "    self.a = 1\n"
        "  }\n"
        "}\n"
        "func main() {\n"
        "  val x: X = X(0)\n"
        "  x.bump()\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_MUT_CALL_ON_VAL));
}

void test_readonly_callable_on_val_binding(void) {
    /* MUTTIER-05: val x = X(); x.readonly_method() is accepted. The
     * readonly method is NOT is_mut_receiver, so MUT-04 never fires;
     * and the caller is main() (not readonly or pure) so no tier check
     * fires either. */
    parse_and_resolve(
        "object X {\n"
        "  var a: Int\n"
        "  readonly func read() -> Int {\n"
        "    return self.a\n"
        "  }\n"
        "}\n"
        "func main() {\n"
        "  val x: X = X(0)\n"
        "  val r: Int = x.read()\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_MUT_CALL_ON_VAL));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_READONLY_CALLS_MUTATING));
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PURE_NON_PURE_CALL));
}

/* ── Phase 85 INIT Plan 85-02 Task 1: definite-assignment + delegation + return-value ── */

void test_init_read_before_assign_emits_E0246(void) {
    /* Reading self.x inside init body BEFORE x is assigned fires E0246. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  var y: Int\n"
        "  init() {\n"
        "    val tmp: Int = self.x\n"
        "    self.x = 1\n"
        "    self.y = 2\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_READ_BEFORE_ASSIGN));
}

void test_init_all_fields_assigned_no_E0247(void) {
    /* init assigns every field on every exit path -> no E0247. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init() {\n"
        "    self.x = 1\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_UNASSIGNED_EXIT));
}

void test_init_missing_field_emits_E0247(void) {
    /* init leaves `y` unassigned on the exit path -> E0247. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  var y: Int\n"
        "  init() {\n"
        "    self.x = 1\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_UNASSIGNED_EXIT));
}

void test_init_missing_on_branch_emits_E0247(void) {
    /* then-branch assigns x, else-branch does not -> x unassigned on merged
     * path -> E0247 at init exit. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(c: Bool) {\n"
        "    if c {\n"
        "      self.x = 1\n"
        "    } else {\n"
        "    }\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_UNASSIGNED_EXIT));
}

void test_init_assigned_on_both_branches_ok(void) {
    /* Both then and else assign x -> merged path assigns x -> no E0247. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(c: Bool) {\n"
        "    if c {\n"
        "      self.x = 1\n"
        "    } else {\n"
        "      self.x = 2\n"
        "    }\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_UNASSIGNED_EXIT));
}

void test_init_val_double_assign_emits_E0248(void) {
    /* val field assigned twice in init body -> E0248 at the second write. */
    parse_and_resolve(
        "object P {\n"
        "  val x: Int\n"
        "  init() {\n"
        "    self.x = 1\n"
        "    self.x = 2\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_VAL_DOUBLE_ASSIGN));
}

void test_init_var_double_assign_ok(void) {
    /* var field assigned twice is legal in init (var writes freely). */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init() {\n"
        "    self.x = 1\n"
        "    self.x = 2\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_VAL_DOUBLE_ASSIGN));
}

void test_init_method_on_partial_self_emits_E0249(void) {
    /* Calling self.helper() while some field still unassigned -> E0249. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  var y: Int\n"
        "  func helper() {}\n"
        "  init() {\n"
        "    self.x = 1\n"
        "    self.helper()\n"
        "    self.y = 2\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_METHOD_ON_PARTIAL));
}

void test_init_method_on_full_self_ok(void) {
    /* Calling self.helper() once all fields assigned -> no E0249. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  func helper() {}\n"
        "  init() {\n"
        "    self.x = 1\n"
        "    self.helper()\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_METHOD_ON_PARTIAL));
}

void test_init_early_return_emits_E0250(void) {
    /* Bare `return` inside an init body while some field is still
     * unassigned -> E0250. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(c: Bool) {\n"
        "    if c {\n"
        "      return\n"
        "    }\n"
        "    self.x = 1\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_EARLY_RETURN));
}

void test_init_bare_return_after_full_assign_ok(void) {
    /* Bare return after all fields assigned is legal -> no E0250. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init() {\n"
        "    self.x = 1\n"
        "    return\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_EARLY_RETURN));
}

void test_init_return_value_emits_E0252(void) {
    /* `return <expr>` inside init body -> E0252 regardless of assignment
     * state. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init() {\n"
        "    self.x = 1\n"
        "    return 5\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_RETURN_VALUE));
}

void test_init_delegation_to_anonymous_emits_E0251(void) {
    /* Inside a named init body, constructing P(...) (the enclosing type)
     * is delegation -> E0251. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "  init alt() {\n"
        "    val tmp: P = P(0)\n"
        "    self.x = 2\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_DELEGATION));
}

void test_init_delegation_to_named_emits_E0251(void) {
    /* Inside an init body, calling P.alt() where alt is a named init on the
     * enclosing type -> E0251. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init() {\n"
        "    val tmp: P = P.alt()\n"
        "    self.x = 1\n"
        "  }\n"
        "  init alt() { self.x = 9 }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_DELEGATION));
}

void test_init_delegation_via_self_init_emits_E0251(void) {
    /* self.init(...) inside any init body -> E0251. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init() {\n"
        "    self.init()\n"
        "    self.x = 1\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_DELEGATION));
}

void test_method_call_Type_args_not_delegation(void) {
    /* INIT-16 regression: non-init method bodies may construct P(args) and
     * must NOT emit E0251. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "  func clone() -> P {\n"
        "    return P(self.x)\n"
        "  }\n"
        "}\n"
        "func main() {}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_DELEGATION));
}

/* ── Phase 85 INIT Plan 85-02 Task 2: call-site dispatch + INIT-16 regression ── */

void test_construct_dispatches_to_anonymous_init(void) {
    /* Explicit anonymous init(v: Int) dispatches from Type(args) call site;
     * type-checks clean. Plan 85-01 stores the anonymous init as
     * method_name="init" + init_name=NULL. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P(42)\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_construct_falls_back_to_v2_2_positional_when_no_init(void) {
    /* Object with no explicit init still accepts v2.2 positional
     * construction. Gate is OFF here so E0264 does not fire; this test
     * verifies that the positional-fallback resolver path still works
     * regardless of the default strict_v3 setting. */
    parse_and_resolve_no_strict(
        "object P {\n"
        "  var x: Int\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P(42)\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_construct_arg_count_checked_against_init_params(void) {
    /* When an explicit anonymous init exists, arg count is checked against
     * init params (excluding synth self) -- NOT against raw field count. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  var y: Int\n"
        "  init(only: Int) {\n"
        "    self.x = only\n"
        "    self.y = 0\n"
        "  }\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P(1, 2)\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_ARG_COUNT));
}

void test_construct_arg_types_checked_against_init_params(void) {
    /* Anonymous init(s: String); caller passes P(42) -- String expected,
     * Int given -> E0217 against the init param type, not the field type. */
    parse_and_resolve(
        "object P {\n"
        "  var x: String\n"
        "  init(s: String) { self.x = s }\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P(42)\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_ARG_TYPE));
}

void test_named_init_dispatch(void) {
    /* P.zero() dispatches to init zero() named init; type-checks clean. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "  init zero() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P.zero()\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_named_init_wrong_args(void) {
    /* P.zero(5) where zero takes no args -> E0216. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "  init zero() { self.x = 0 }\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P.zero(5)\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_ARG_COUNT));
}

void test_named_init_unknown_name_falls_through(void) {
    /* P.not_a_real_init() — <not_a_real_init> is not a declared init and
     * there is no classic method by that name, so the existing method
     * resolver emits its usual diagnostic (NOT a new init-specific error).
     * The test asserts we do NOT emit E0251 for an unknown name (no
     * spurious delegation); any other diagnostic is fine. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "}\n"
        "func main() {\n"
        "  val p: P = P.not_a_real_init()\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_DELEGATION));
}

void test_method_body_can_call_Type_args(void) {
    /* INIT-16 regression: non-init method body constructs via P(self.x)
     * with explicit init present -> type-checks clean; no E0251. readonly
     * on clone() prevents the Phase 80 mut-receiver-on-val diagnostic
     * which is orthogonal to INIT-16. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "  readonly func clone() -> P {\n"
        "    return P(self.x)\n"
        "  }\n"
        "}\n"
        "func main() {\n"
        "  val a: P = P(1)\n"
        "  val b: P = a.clone()\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_DELEGATION));
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_method_body_can_call_Type_name_args(void) {
    /* INIT-16 regression: non-init method body calls P.zero() -> no E0251,
     * no other errors. */
    parse_and_resolve(
        "object P {\n"
        "  var x: Int\n"
        "  init(v: Int) { self.x = v }\n"
        "  init zero() { self.x = 0 }\n"
        "  readonly func fresh() -> P {\n"
        "    return P.zero()\n"
        "  }\n"
        "}\n"
        "func main() {\n"
        "  val a: P = P(1)\n"
        "  val b: P = a.fresh()\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_INIT_DELEGATION));
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Phase 86 Plan 86-02 Task 1 RED: patch registry + primitive targets + E0254 ── */

void test_patch_registry_registers_user_object(void) {
    /* PATCH-02: patches on user objects register against the target type. */
    Iron_Program *prog = parse_and_resolve_only(
        "object Foo {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "patch object Foo {\n"
        "    pub readonly func describe() -> Int { return self.x }\n"
        "}\n"
    );
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PATCH_TARGET_NOT_FOUND));

    Iron_TypePatchRegistry *reg =
        iron_type_patch_registry_build(prog, g_global_scope_for_patch,
                                       &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(reg);
    Iron_MethodDecl **methods = iron_type_patch_lookup(reg, "Foo");
    TEST_ASSERT_NOT_NULL(methods);
    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(methods));
    TEST_ASSERT_EQUAL_STRING("describe", methods[0]->method_name);
    TEST_ASSERT_EQUAL_STRING("Foo", methods[0]->type_name);
    iron_type_patch_registry_free(reg);
}

void test_patch_registry_registers_primitive(void) {
    /* PATCH-04: primitives (Int, Int32, Float, String, Bool) are valid
     * targets without a user-declared object of the same name. */
    Iron_Program *prog = parse_and_resolve_only(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
    );
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PATCH_TARGET_NOT_FOUND));

    Iron_TypePatchRegistry *reg =
        iron_type_patch_registry_build(prog, g_global_scope_for_patch,
                                       &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(reg);
    Iron_MethodDecl **methods = iron_type_patch_lookup(reg, "Int");
    TEST_ASSERT_NOT_NULL(methods);
    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(methods));
    TEST_ASSERT_EQUAL_STRING("double", methods[0]->method_name);
    iron_type_patch_registry_free(reg);
}

void test_patch_unknown_target_emits_e0254(void) {
    /* PATCH-04: `patch object Xyzzy` with no user type and no primitive
     * match emits IRON_ERR_PATCH_TARGET_NOT_FOUND with 'patch target'
     * in the message. */
    Iron_Program *prog = parse_and_resolve_only(
        "patch object Xyzzy {\n"
        "    func foo() -> Int { return 1 }\n"
        "}\n"
    );
    TEST_ASSERT_NOT_NULL(prog);

    Iron_TypePatchRegistry *reg =
        iron_type_patch_registry_build(prog, g_global_scope_for_patch,
                                       &g_arena, &g_diags);
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PATCH_TARGET_NOT_FOUND));
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("patch target"),
        "expected locked 'patch target' substring in E0254 message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("Xyzzy"),
        "expected unknown target name 'Xyzzy' in E0254 message");
    iron_type_patch_registry_free(reg);
}

void test_patch_multiple_patches_same_target(void) {
    /* Task 1 alone does not run the collision scan — two distinct methods
     * on the same target both register, lookup returns a 2-element array. */
    Iron_Program *prog = parse_and_resolve_only(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
        "patch object Int {\n"
        "    pub readonly func triple() -> Int { return self }\n"
        "}\n"
    );
    TEST_ASSERT_NOT_NULL(prog);
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PATCH_TARGET_NOT_FOUND));

    Iron_TypePatchRegistry *reg =
        iron_type_patch_registry_build(prog, g_global_scope_for_patch,
                                       &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(reg);
    Iron_MethodDecl **methods = iron_type_patch_lookup(reg, "Int");
    TEST_ASSERT_NOT_NULL(methods);
    TEST_ASSERT_EQUAL_INT(2, (int)arrlen(methods));
    iron_type_patch_registry_free(reg);
}

void test_patch_primitive_names_allowlist(void) {
    /* Each primitive in the allowlist (Int, Int32, Float, String, Bool)
     * accepts a patch without E0254. */
    Iron_Program *prog = parse_and_resolve_only(
        "patch object Int    { pub readonly func a() -> Int { return self } }\n"
        "patch object Int32  { pub readonly func a() -> Int { return 1    } }\n"
        "patch object Float  { pub readonly func a() -> Int { return 1    } }\n"
        "patch object String { pub readonly func a() -> Int { return 1    } }\n"
        "patch object Bool   { pub readonly func a() -> Int { return 1    } }\n"
    );
    TEST_ASSERT_NOT_NULL(prog);

    Iron_TypePatchRegistry *reg =
        iron_type_patch_registry_build(prog, g_global_scope_for_patch,
                                       &g_arena, &g_diags);
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_PATCH_TARGET_NOT_FOUND),
        "primitives Int/Int32/Float/String/Bool must NOT emit E0254");

    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(iron_type_patch_lookup(reg, "Int")));
    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(iron_type_patch_lookup(reg, "Int32")));
    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(iron_type_patch_lookup(reg, "Float")));
    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(iron_type_patch_lookup(reg, "String")));
    TEST_ASSERT_EQUAL_INT(1, (int)arrlen(iron_type_patch_lookup(reg, "Bool")));
    iron_type_patch_registry_free(reg);
}

/* ── Phase 86 Plan 86-02 Task 2 RED: collision scan + dispatch + regression locks ── */

void test_patch_patch_collision(void) {
    /* PATCH-03: two patches on the same target declaring the same method
     * name fire IRON_ERR_PATCH_CONFLICT with 'conflicting patch' locked
     * in the message. */
    parse_and_resolve(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PATCH_CONFLICT));
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("conflicting patch"),
        "expected 'conflicting patch' substring in E0255 message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("Int.double"),
        "expected 'Int.double' to appear in E0255 message");
}

void test_patch_object_collision(void) {
    /* PATCH-06: patch method name collides with existing in-object method
     * -> same IRON_ERR_PATCH_CONFLICT. */
    parse_and_resolve(
        "object Foo {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "    pub readonly func bar() -> Int { return self.x }\n"
        "}\n"
        "patch object Foo {\n"
        "    pub readonly func bar() -> Int { return self.x }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PATCH_CONFLICT));
}

void test_patch_collision_across_different_targets_no_error(void) {
    /* Different targets never collide even if the method name matches. */
    parse_and_resolve(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
        "patch object Float {\n"
        "    pub readonly func double() -> Int { return 1 }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PATCH_CONFLICT));
}

void test_patch_collision_different_names_no_error(void) {
    /* Same target, different method names -> no collision. */
    parse_and_resolve(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
        "patch object Int {\n"
        "    pub readonly func triple() -> Int { return self }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE(has_error(IRON_ERR_PATCH_CONFLICT));
}

void test_patch_method_call_dispatches_through_registry_user(void) {
    /* PATCH-02 dispatch: object Foo + patch-Foo.describe resolves
     * mc->resolved_type on `f.describe()` to Int. */
    parse_and_resolve(
        "object Foo {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "patch object Foo {\n"
        "    pub readonly func describe() -> Int { return self.x }\n"
        "}\n"
        "func main() {\n"
        "    val f: Foo = Foo(7)\n"
        "    val r: Int = f.describe()\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_patch_method_call_dispatches_through_registry_primitive(void) {
    /* PATCH-04 dispatch: patch-Int.double typechecks 5.double() with
     * resolved_type Int. */
    parse_and_resolve(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
        "func main() {\n"
        "    val r: Int = 5.double()\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_patch_named_init_dispatches(void) {
    /* Patched named init Foo.from_x(7) dispatches through the registry. */
    parse_and_resolve(
        "object Foo {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "patch object Foo {\n"
        "    init from_x(v: Int) { self.x = v }\n"
        "}\n"
        "func main() {\n"
        "    val f: Foo = Foo.from_x(7)\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

void test_patched_init_unassigned_field(void) {
    /* PATCH-07 regression: patched init that leaves a field unassigned
     * fires E0247 (same branch as in-object inits). */
    parse_and_resolve(
        "object Foo {\n"
        "    var x: Int\n"
        "    var y: Int\n"
        "    init(v: Int) { self.x = v\n self.y = 0 }\n"
        "}\n"
        "patch object Foo {\n"
        "    init only_x(v: Int) { self.x = v }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_INIT_UNASSIGNED_EXIT));
}

void test_patched_readonly_method_write_self_rejected(void) {
    /* PATCH-09 regression: patched readonly method writing self.field
     * fires E0238 (same branch as in-object readonly methods). */
    parse_and_resolve(
        "object Foo {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "patch object Foo {\n"
        "    readonly func broken() { self.x = 1 }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_READONLY_WRITE_SELF));
}

void test_patched_pure_method_io_rejected(void) {
    /* PATCH-09 regression: patched pure method calling an I/O builtin
     * fires E0240 (same branch as in-object pure methods). */
    parse_and_resolve(
        "object Foo {\n"
        "    var x: Int\n"
        "    init(v: Int) { self.x = v }\n"
        "}\n"
        "patch object Foo {\n"
        "    pure func broken() { println(\"bad\") }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE(has_error(IRON_ERR_PURE_IO));
}

/* ── Phase 87 Plan 87-01 Task 2: IFACE-02 tier-strengthening (E0257) ─────── */
/* Tests 9-18 covering the 3x3 tier matrix (iface default/readonly/pure x
 * impl default/readonly/pure), default-body inheritance, and locked
 * message substring verification. */

void test_iface_readonly_impl_default_mutating_rejected(void) {
    /* IFACE-02: readonly iface sig + mutating impl => E0257.
     * Implementation is an in-object in-block method (Phase 82) so it gets
     * a MethodDecl with is_readonly=false, which is the mismatch. */
    parse_and_resolve(
        "interface Cmp {\n"
        "    readonly func cmp() -> Int\n"
        "}\n"
        "object Foo impl Cmp {\n"
        "    init() {}\n"
        "    func cmp() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "expected E0257 for mutating impl of readonly iface method");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("requires readonly"),
        "expected 'requires readonly' in E0257 message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("implementation is"),
        "expected 'implementation is' in E0257 message");
}

void test_iface_pure_impl_readonly_rejected(void) {
    /* IFACE-02: pure iface sig + readonly impl => E0257.
     * Uses in-block readonly method (Phase 82/84) so is_readonly=true but
     * is_pure=false — weaker than required pure. */
    parse_and_resolve(
        "interface Hash {\n"
        "    pure func hash() -> Int\n"
        "}\n"
        "object Foo impl Hash {\n"
        "    init() {}\n"
        "    readonly func hash() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "expected E0257 for readonly impl of pure iface method");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("requires pure"),
        "expected 'requires pure' in E0257 message");
}

void test_iface_pure_impl_default_rejected(void) {
    /* IFACE-02: pure iface sig + mutating impl => E0257. */
    parse_and_resolve(
        "interface Hash {\n"
        "    pure func hash() -> Int\n"
        "}\n"
        "object Bar impl Hash {\n"
        "    init() {}\n"
        "    func hash() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "expected E0257 for mutating impl of pure iface method");
}

void test_iface_readonly_impl_readonly_accepted(void) {
    /* IFACE-02: readonly iface sig + readonly impl => zero E0257. */
    parse_and_resolve(
        "interface Cmp {\n"
        "    readonly func cmp() -> Int\n"
        "}\n"
        "object Foo impl Cmp {\n"
        "    init() {}\n"
        "    readonly func cmp() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "E0257 should NOT fire for readonly impl of readonly iface method");
}

void test_iface_readonly_impl_pure_accepted(void) {
    /* IFACE-02: readonly iface sig + pure impl => zero E0257 (pure >= readonly). */
    parse_and_resolve(
        "interface Cmp {\n"
        "    readonly func cmp() -> Int\n"
        "}\n"
        "object Foo impl Cmp {\n"
        "    init() {}\n"
        "    pure func cmp() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "E0257 should NOT fire for pure impl of readonly iface method");
}

void test_iface_pure_impl_pure_accepted(void) {
    /* IFACE-02: pure iface sig + pure impl => zero E0257. */
    parse_and_resolve(
        "interface Hash {\n"
        "    pure func hash() -> Int\n"
        "}\n"
        "object Foo impl Hash {\n"
        "    init() {}\n"
        "    pure func hash() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "E0257 should NOT fire for pure impl of pure iface method");
}

void test_iface_default_impl_mutating_accepted(void) {
    /* IFACE-02: default iface sig + mutating impl => zero E0257. */
    parse_and_resolve(
        "interface Base {\n"
        "    func run()\n"
        "}\n"
        "object Foo impl Base {\n"
        "    init() {}\n"
        "    func run() { return }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "E0257 should NOT fire for mutating impl of default iface method");
}

void test_iface_default_impl_readonly_accepted(void) {
    /* IFACE-02: default iface sig + readonly impl => zero E0257. */
    parse_and_resolve(
        "interface Base {\n"
        "    func run()\n"
        "}\n"
        "object Foo impl Base {\n"
        "    init() {}\n"
        "    readonly func run() { return }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "E0257 should NOT fire for readonly impl of default iface method");
}

void test_iface_default_body_inherited_no_mismatch(void) {
    /* IFACE-03/02 interaction: implementer has no override for a default-body
     * method. No impl MethodDecl exists => no tier comparison => zero E0257. */
    parse_and_resolve(
        "interface D {\n"
        "    readonly func foo() -> Int { return 42 }\n"
        "}\n"
        "object Foo impl D {\n"
        "    init() {}\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "E0257 should NOT fire when implementer inherits the default body");
}

void test_iface_tier_mismatch_E0257_locked_substring(void) {
    /* Lock the exact ASCII substrings that Plan 87-03 compile_fail fixtures
     * will grep: "interface method" + "requires" + "implementation is". */
    parse_and_resolve(
        "interface Sig {\n"
        "    readonly func go() -> Int\n"
        "}\n"
        "object Impl impl Sig {\n"
        "    init() {}\n"
        "    func go() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("interface method"),
        "expected 'interface method' prefix in E0257 message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("requires"),
        "expected 'requires' in E0257 message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("implementation is"),
        "expected 'implementation is' suffix in E0257 message");
}

/* ── Phase 87-02 SELF-01/02/03 + IFACE-05: Self type resolution ───────────── */

/* test_self_return_resolves_to_enclosing: method with `-> Self` return type;
 * the resolved_return_type should be the Point object type. */
void test_self_return_resolves_to_enclosing(void) {
    /* Use var (not pub val) to avoid the pub-val init-assignment restriction. */
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "object Point {\n"
        "    var x: Int\n"
        "    init(x: Int) { self.x = x }\n"
        "    readonly func double_x() -> Self { return Self(self.x * 2) }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)prog->decls[i];
            if (strcmp(md->method_name, "double_x") == 0) {
                TEST_ASSERT_NOT_NULL(md->resolved_return_type);
                TEST_ASSERT_EQUAL_INT(IRON_TYPE_OBJECT, md->resolved_return_type->kind);
                TEST_ASSERT_EQUAL_STRING("Point", md->resolved_return_type->object.decl->name);
                return;
            }
        }
    }
    TEST_FAIL_MESSAGE("double_x MethodDecl not found");
}

/* test_self_named_init_resolves: method `Self.from_x(v)` resolves to Point type.
 * Uses var (not pub val) to avoid the pub-val init-assignment restriction. */
void test_self_named_init_resolves(void) {
    parse_and_resolve(
        "object Point {\n"
        "    var x: Int\n"
        "    init from_x(x: Int) { self.x = x }\n"
        "    readonly func make_shifted(o: Int) -> Self { return Self.from_x(self.x + o) }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* test_self_outside_method_rejected: `func free_fn() -> Self` emits E0259. */
void test_self_outside_method_rejected(void) {
    parse_and_resolve(
        "func free_fn() -> Self { return 0 }\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_SELF_OUTSIDE_CONTEXT),
        "expected E0259 for Self in free function return type");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("'Self' is only valid"),
        "expected locked substring \"'Self' is only valid\" in E0259 message");
}

/* test_self_return_inside_init: `func make_self() -> Self { return Self() }` inside
 * object with parameterless init: zero diags, return type is the object. */
void test_self_return_inside_init(void) {
    parse_and_resolve(
        "object P {\n"
        "    init() {}\n"
        "    func make_self() -> Self { return Self() }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* test_self_in_interface_sig_bound_to_implementer: Cat impl Clone where
 * Clone has `readonly func clone() -> Self`; Cat.clone returns Cat; no E0257. */
void test_self_in_interface_sig_bound_to_implementer(void) {
    parse_and_resolve(
        "interface Clone {\n"
        "    readonly func clone() -> Self\n"
        "}\n"
        "object Cat impl Clone {\n"
        "    init() {}\n"
        "    readonly func clone() -> Cat { return self }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* test_self_in_nested_method_resolves_correctly: Self inside A resolves to A,
 * Self inside B resolves to B — no cross-object bleed.
 * Uses var fields to avoid the pub-val init-assignment restriction. */
void test_self_in_nested_method_resolves_correctly(void) {
    parse_and_resolve(
        "object A {\n"
        "    var x: Int\n"
        "    init(x: Int) { self.x = x }\n"
        "    readonly func copy() -> Self { return Self(self.x) }\n"
        "}\n"
        "object B {\n"
        "    var y: Int\n"
        "    init(y: Int) { self.y = y }\n"
        "    readonly func copy() -> Self { return Self(self.y) }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── Phase 87-02 PATCH-08: retroactive conformance + E0258 ───────────────── */

/* test_patch_retroactive_conformance_full: patch declares implements and provides
 * all required interface methods => zero E0258. */
void test_patch_retroactive_conformance_full(void) {
    parse_and_resolve(
        "interface Comparable {\n"
        "    readonly func cmp(other: Int) -> Int\n"
        "}\n"
        "patch object Int implements Comparable {\n"
        "    readonly func cmp(other: Int) -> Int { return self - other }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_CONFORMANCE_MISSING),
        "E0258 should NOT fire when patch provides all required interface methods");
}

/* test_patch_retroactive_missing_method_e0258: patch declares implements but body
 * has no methods => E0258 with locked substrings. */
void test_patch_retroactive_missing_method_e0258(void) {
    parse_and_resolve(
        "interface Comparable {\n"
        "    readonly func cmp(other: Int) -> Int\n"
        "}\n"
        "patch object Int implements Comparable {\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_IFACE_CONFORMANCE_MISSING),
        "expected E0258 when patch declares implements but omits required method");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("missing interface method"),
        "expected locked substring 'missing interface method' in E0258 message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("cmp"),
        "expected method name 'cmp' in E0258 message");
}

/* test_patch_retroactive_partial_coverage: interface with 2 methods, patch provides
 * only 1 => E0258 fires exactly once for the missing method. */
void test_patch_retroactive_partial_coverage(void) {
    parse_and_resolve(
        "interface TwoMethods {\n"
        "    readonly func foo() -> Int\n"
        "    readonly func bar() -> Int\n"
        "}\n"
        "patch object Int implements TwoMethods {\n"
        "    readonly func foo() -> Int { return 0 }\n"
        "}\n"
    );
    int e258_count = 0;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == IRON_ERR_IFACE_CONFORMANCE_MISSING) {
            e258_count++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, e258_count,
        "expected exactly 1 E0258 for the one missing method");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("bar"),
        "expected missing method name 'bar' in E0258 message");
}

/* test_patch_retroactive_with_default_body: interface method has a default body;
 * patch declares implements but provides no override => zero E0258. */
void test_patch_retroactive_with_default_body(void) {
    parse_and_resolve(
        "interface HasDefault {\n"
        "    readonly func greet() -> Int { return 42 }\n"
        "}\n"
        "patch object Int implements HasDefault {\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_CONFORMANCE_MISSING),
        "E0258 should NOT fire when interface method has a default body");
}

/* test_patch_retroactive_tier_strengthening: interface has readonly method; patch
 * provides a mutating impl => E0257 fires (tier mismatch, not E0258). */
void test_patch_retroactive_tier_strengthening(void) {
    parse_and_resolve(
        "interface ReadOnly {\n"
        "    readonly func get() -> Int\n"
        "}\n"
        "patch object Int implements ReadOnly {\n"
        "    func get() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_IFACE_METHOD_TIER_MISMATCH),
        "expected E0257 for mutating patch impl of readonly iface method");
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_CONFORMANCE_MISSING),
        "E0258 should NOT fire when method is present but has wrong tier");
}

/* test_classic_object_conformance_still_works: classic object declares impl and
 * provides all required methods => zero E0258 (regression). */
void test_classic_object_conformance_still_works(void) {
    parse_and_resolve(
        "interface Runnable {\n"
        "    func run()\n"
        "}\n"
        "object Worker impl Runnable {\n"
        "    init() {}\n"
        "    func run() { return }\n"
        "}\n"
    );
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_IFACE_CONFORMANCE_MISSING),
        "E0258 should NOT fire for classic object satisfying all interface methods");
}

/* test_classic_object_missing_method_emits_e0258: classic object declares impl but
 * omits a required interface method => E0258 fires. */
void test_classic_object_missing_method_emits_e0258(void) {
    parse_and_resolve(
        "interface Runnable {\n"
        "    func run()\n"
        "}\n"
        "object Worker impl Runnable {\n"
        "    init() {}\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_IFACE_CONFORMANCE_MISSING),
        "expected E0258 for classic object missing required interface method");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("missing interface method"),
        "expected locked substring 'missing interface method' in E0258 message");
}

/* ── Phase 93 VIS-01: top-level `pub` parser threading ──────────────────── */

void test_v93_pub_func_top_level_parses(void) {
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "pub func foo() -> Int { return 1 }\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    Iron_FuncDecl *fd = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_FUNC_DECL) {
            fd = (Iron_FuncDecl *)prog->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(fd);
    TEST_ASSERT_TRUE(fd->is_pub);
    TEST_ASSERT_FALSE(fd->is_private);
}

void test_v93_top_level_func_default_private(void) {
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "func bar() {}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    Iron_FuncDecl *fd = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_FUNC_DECL) {
            fd = (Iron_FuncDecl *)prog->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(fd);
    TEST_ASSERT_FALSE(fd->is_pub);
}

void test_v93_pub_object_parses(void) {
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "pub object Player { var x: Int  init(x: Int) { self.x = x } }\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    Iron_ObjectDecl *od = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *cand = (Iron_ObjectDecl *)prog->decls[i];
            if (cand->name && strcmp(cand->name, "Player") == 0) {
                od = cand;
                break;
            }
        }
    }
    TEST_ASSERT_NOT_NULL(od);
    TEST_ASSERT_TRUE(od->is_pub);
}

void test_v93_pub_enum_parses(void) {
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "pub enum State { RUNNING, IDLE }\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    Iron_EnumDecl *ed = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_ENUM_DECL) {
            ed = (Iron_EnumDecl *)prog->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(ed);
    TEST_ASSERT_TRUE(ed->is_pub);
}

void test_v93_pub_patch_object_parses(void) {
    /* Use a user-defined object as the patch target so the receiver-type
     * check (E0236 on primitive `Int` mut-receiver) does not fire. The
     * point of this test is the parser threading of `pub` through
     * iron_parse_patch_decl, not patch semantics. */
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "object Foo { var x: Int  init(x: Int) { self.x = x } }\n"
        "pub patch object Foo { func twice() -> Int { return self.x * 2 } }\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    Iron_ObjectDecl *od = NULL;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *cand = (Iron_ObjectDecl *)prog->decls[i];
            if (cand->is_patch) {
                od = cand;
                break;
            }
        }
    }
    TEST_ASSERT_NOT_NULL(od);
    TEST_ASSERT_TRUE(od->is_patch);
    TEST_ASSERT_TRUE(od->is_pub);
}

void test_v93_pub_private_combined_rejected(void) {
    parse_and_resolve("pub private func foo() {}\n");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("`pub` and `private` cannot be combined"),
        "expected pub+private mutual-exclusion error");
}

void test_v93_top_level_pub_init_rejected(void) {
    parse_and_resolve("pub init Foo() {}\n");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("standalone `pub init`"),
        "expected standalone-pub-init rejection");
}

void test_v93_top_level_pub_val_rejected(void) {
    parse_and_resolve("pub val GLOBAL: Int = 0\n");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("top-level `pub val`"),
        "expected top-level pub val rejection");
}

/* Phase 93 VIS-04 (Plan 93-02): `pub init` gating on enclosing-object visibility. */

void test_v93_pub_init_in_pub_object_accepted(void) {
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "pub object Player {\n"
        "    var hp: Int\n"
        "    pub init(hp: Int) { self.hp = hp }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    bool found_pub_init = false;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *m = (Iron_MethodDecl *)prog->decls[i];
            if (m->method_name && strcmp(m->method_name, "init") == 0 && m->is_pub) {
                found_pub_init = true;
                break;
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_pub_init,
        "expected a synthesized init MethodDecl with is_pub == true");
}

void test_v93_pub_init_in_private_object_rejected(void) {
    parse_and_resolve(
        "object Foo {\n"
        "    pub init() {}\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("`pub init` is only valid inside a `pub object`"),
        "expected the new pub-init-in-private-object message");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("the enclosing"),
        "expected message to point at the enclosing object");
    /* Locks the message-shape contract: subsequent compile_fail fixture in
     * Plan 93-04 substring-matches against this exact phrasing. */
}

void test_v93_plain_init_in_pub_object_stays_private(void) {
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "pub object Bar {\n"
        "    init() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    /* CONTEXT: pub on the type does NOT propagate to members. The plain
     * init MethodDecl must have is_pub == false even though the enclosing
     * object is pub. Cross-module `Bar()` construction still works because
     * init dispatch goes through method-table lookup, not top-level
     * identifier resolution (RESEARCH Pitfall 4). */
    bool found_init = false;
    for (int i = 0; i < prog->decl_count; i++) {
        if (prog->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *m = (Iron_MethodDecl *)prog->decls[i];
            if (m->method_name && strcmp(m->method_name, "init") == 0) {
                TEST_ASSERT_FALSE_MESSAGE(m->is_pub,
                    "plain init in pub object must NOT auto-promote to pub");
                found_init = true;
            }
        }
    }
    TEST_ASSERT_TRUE(found_init);
}

/* ── Phase 93 VIS-02/03 (Plan 93-03): resolver-side is_pub propagation +
 *    E0320 cross-module visibility check + stdlib carve-out ──────────────── */

/* Lookup helper: walk a global scope's stb_ds shmap and find the named symbol.
 * Phase 93-03 uses this to assert that resolver-stage propagation copies the
 * AST is_pub bit onto Iron_Symbol.is_pub. */
static Iron_Symbol *find_global_sym(Iron_Scope *global, const char *name) {
    if (!global || !name) return NULL;
    for (ptrdiff_t i = 0; i < shlen(global->symbols); i++) {
        if (global->symbols[i].key && strcmp(global->symbols[i].key, name) == 0) {
            return global->symbols[i].value;
        }
    }
    return NULL;
}

void test_v93_e0320_macro_registered(void) {
    /* IRON_ERR_CROSS_MODULE_PRIVATE must be the agreed slot 320, between
     * E0314 (POSSIBLY_UNINITIALIZED) and E0400 (LOWER_UNSUPPORTED). */
    TEST_ASSERT_EQUAL_INT(320, IRON_ERR_CROSS_MODULE_PRIVATE);
}

void test_v93_sym_is_pub_propagates_func(void) {
    /* `pub func` -> Iron_Symbol.is_pub == true. */
    const char *src = "pub func foo() -> Int { return 1 }\n";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron",
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);
    Iron_Symbol *sym = find_global_sym(global, "foo");
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "expected `foo` symbol in global scope");
    TEST_ASSERT_TRUE_MESSAGE(sym->is_pub,
        "expected sym->is_pub == true for `pub func foo`");
}

void test_v93_sym_is_pub_default_false_func(void) {
    /* Plain `func bar` -> Iron_Symbol.is_pub == false. */
    const char *src = "func bar() {}\n";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron",
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);
    Iron_Symbol *sym = find_global_sym(global, "bar");
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "expected `bar` symbol in global scope");
    TEST_ASSERT_FALSE_MESSAGE(sym->is_pub,
        "expected sym->is_pub == false for plain `func bar`");
}

void test_v93_sym_is_pub_propagates_enum_and_variants(void) {
    /* `pub enum State { RUNNING IDLE }` -> enum sym + each variant sym
     * are all is_pub == true. */
    const char *src = "pub enum State { RUNNING, IDLE }\n";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron",
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);

    Iron_Symbol *enum_sym = find_global_sym(global, "State");
    TEST_ASSERT_NOT_NULL_MESSAGE(enum_sym, "expected `State` enum sym");
    TEST_ASSERT_TRUE_MESSAGE(enum_sym->is_pub,
        "expected pub enum sym->is_pub == true");

    Iron_Symbol *running_sym = find_global_sym(global, "RUNNING");
    TEST_ASSERT_NOT_NULL_MESSAGE(running_sym, "expected `RUNNING` variant sym");
    TEST_ASSERT_TRUE_MESSAGE(running_sym->is_pub,
        "expected variant of pub enum to inherit is_pub == true");

    Iron_Symbol *idle_sym = find_global_sym(global, "IDLE");
    TEST_ASSERT_NOT_NULL_MESSAGE(idle_sym, "expected `IDLE` variant sym");
    TEST_ASSERT_TRUE_MESSAGE(idle_sym->is_pub,
        "expected variant of pub enum to inherit is_pub == true");
}

void test_v93_sym_is_pub_default_false_enum_variants(void) {
    /* `enum Color { RED }` -> variant `RED` is_pub == false. */
    const char *src = "enum Color { RED }\n";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron",
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);

    Iron_Symbol *red_sym = find_global_sym(global, "RED");
    TEST_ASSERT_NOT_NULL_MESSAGE(red_sym, "expected `RED` variant sym");
    TEST_ASSERT_FALSE_MESSAGE(red_sym->is_pub,
        "expected variant of plain enum to have is_pub == false");
}

void test_v93_sym_is_pub_propagates_object(void) {
    /* `pub object Player` -> Iron_Symbol.is_pub == true. */
    const char *src =
        "pub object Player { var x: Int  init(x: Int) { self.x = x } }\n";
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron",
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;
    Iron_Scope   *global = iron_resolve(prog, &g_arena, &g_diags);

    Iron_Symbol *sym = find_global_sym(global, "Player");
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "expected `Player` type sym");
    TEST_ASSERT_TRUE_MESSAGE(sym->is_pub,
        "expected pub object sym->is_pub == true");
}

/* ── Phase 93 VIS-03 (Plan 93-03): cross-module visibility check at the
 *    IRON_NODE_IDENT lookup site, plus rate-limited audit-grep note. ─────── */

/* parse_and_resolve_two_files: simulate cross-module by directly synthesizing
 * a small program where two top-level decls carry distinct span.filename
 * values. The parse_and_resolve helper above uses a single source string with
 * one filename, which is structurally same-file. To exercise the cross-module
 * gate (decl_file != use_file), we build the input as a single source string
 * and then post-walk the resulting AST to retag the second function's decls
 * (and the IDENT use-site) with a different filename. This sidesteps the
 * Plan 04 multi-file test harness and locks the resolver-side check
 * independently. */
static Iron_Program *parse_two_files_then_resolve(
        const char *file_a_filename, const char *file_a_src,
        const char *file_b_filename, const char *file_b_src) {
    /* Concatenate the two files with a marker we can scan for to find where
     * file B starts; then parse the combined source under file_a_filename
     * and post-walk to retag file B's decls + their bodies' span.filename. */
    size_t la = strlen(file_a_src);
    size_t lb = strlen(file_b_src);
    char *combined = (char *)iron_arena_alloc(&g_arena, la + 1 + lb + 1, 1);
    memcpy(combined, file_a_src, la);
    combined[la] = '\n';
    memcpy(combined + la + 1, file_b_src, lb);
    combined[la + 1 + lb] = '\0';

    Iron_Lexer   l      = iron_lexer_create(combined, file_a_filename, &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;
    Iron_Parser  p = iron_parser_create(tokens, count, combined, file_a_filename,
                                         &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    Iron_Program *prog = (Iron_Program *)root;

    /* Compute the line where file B begins in the combined source: count '\n'
     * bytes in file_a_src + 1 (for the separator) -> that is the 1-indexed
     * line number of the first character of file_b_src. */
    int a_newlines = 0;
    for (size_t i = 0; i < la; i++) if (file_a_src[i] == '\n') a_newlines++;
    int file_b_start_line = a_newlines + 2;  /* +1 for separator, +1 for 1-indexed */

    /* Retag every AST node whose span is on or after file_b_start_line to
     * have filename = file_b_filename. The walk only needs to hit top-level
     * decls' spans + their inner identifier use-site spans; keep it simple
     * by scanning Iron_Program's decl array and walking decl.body if present. */
    const char *intern_b =
        iron_arena_strdup(&g_arena, file_b_filename, strlen(file_b_filename));
    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *d = prog->decls[i];
        if (!d) continue;
        if (d->span.line >= (uint32_t)file_b_start_line) {
            d->span.filename = intern_b;
            /* Walk the body block (if any) and retag its statements. */
            Iron_Node *body = NULL;
            if (d->kind == IRON_NODE_FUNC_DECL) {
                body = ((Iron_FuncDecl *)d)->body;
            } else if (d->kind == IRON_NODE_METHOD_DECL) {
                body = ((Iron_MethodDecl *)d)->body;
            }
            if (body && body->kind == IRON_NODE_BLOCK) {
                Iron_Block *b = (Iron_Block *)body;
                b->span.filename = intern_b;
                for (int j = 0; j < b->stmt_count; j++) {
                    if (!b->stmts[j]) continue;
                    b->stmts[j]->span.filename = intern_b;
                    /* For ExprStmt -> Call(callee=Ident, ...), retag the
                     * callee Ident's span too so the IDENT resolve site sees
                     * the file_b filename. */
                    if (b->stmts[j]->kind == IRON_NODE_CALL) {
                        Iron_CallExpr *ce = (Iron_CallExpr *)b->stmts[j];
                        if (ce->callee) ce->callee->span.filename = intern_b;
                    }
                }
            }
        }
    }

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    (void)global;
    return prog;
}

void test_v93_e0320_cross_module_private_rejected(void) {
    /* file_a.iron declares a private (default) `helper`; file_b.iron calls
     * helper(). Expected: E0320 with substring "not visible from". */
    Iron_Program *prog = parse_two_files_then_resolve(
        "lib.iron",
        "func helper() -> Int { return 42 }\n",
        "main.iron",
        "func main() { helper() }\n"
    );
    (void)prog;
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_CROSS_MODULE_PRIVATE),
        "expected E0320 IRON_ERR_CROSS_MODULE_PRIVATE for cross-module private ref");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("not visible from"),
        "expected E0320 message substring 'not visible from'");
}

void test_v93_e0320_cross_module_pub_accepted(void) {
    /* `pub func helper` is visible across files -> NO E0320. */
    Iron_Program *prog = parse_two_files_then_resolve(
        "lib.iron",
        "pub func helper() -> Int { return 42 }\n",
        "main.iron",
        "func main() { helper() }\n"
    );
    (void)prog;
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_CROSS_MODULE_PRIVATE),
        "pub-marked symbol must not trigger E0320 across modules");
}

void test_v93_e0320_same_file_private_accepted(void) {
    /* Same-file references to non-pub symbols never trigger E0320. */
    Iron_Program *prog = (Iron_Program *)parse_and_resolve(
        "func helper() -> Int { return 42 }\n"
        "func main() { helper() }\n"
    );
    (void)prog;
    TEST_ASSERT_FALSE_MESSAGE(
        has_error(IRON_ERR_CROSS_MODULE_PRIVATE),
        "same-file private reference must not trigger E0320");
}

void test_v93_e0320_audit_note_rate_limited(void) {
    /* Two cross-module references in the same compile unit should both emit
     * E0320 (count == 2), but the audit-grep note line ("audit your public
     * surface") must appear in only ONE of those diagnostics' suggestion
     * fields. */
    Iron_Program *prog = parse_two_files_then_resolve(
        "lib.iron",
        "func helper_a() -> Int { return 1 }\n"
        "func helper_b() -> Int { return 2 }\n",
        "main.iron",
        "func main() { helper_a()  helper_b() }\n"
    );
    (void)prog;

    int e0320_count = 0;
    int audit_note_count = 0;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == IRON_ERR_CROSS_MODULE_PRIVATE) {
            e0320_count++;
            if (g_diags.items[i].suggestion &&
                strstr(g_diags.items[i].suggestion, "audit your public surface")) {
                audit_note_count++;
            }
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, e0320_count,
        "expected at least 2 E0320 diagnostics for two cross-module refs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, audit_note_count,
        "audit-grep note must appear at most once across all E0320 diags");
}

/* ── Phase 93 Plan 04 Task 3: compile_fail fixture substring locks ───────────
 *
 * tests/compile_fail/ is NOT auto-discovered by tests/run_tests.sh (Phase 80
 * commit 3bc6329 convention). Phase 87 IFACE convention (test_typecheck.c
 * around line 2727) wires each compile_fail fixture into CI by pairing it
 * with a unit test that runs parse_and_resolve(...) on equivalent inline
 * source and asserts has_error_msg_substring(...) on the locked text from
 * the fixture's `.expected` sibling. Each test below is the CI gate for
 * one tests/compile_fail/v3_*.iron + .expected pair. */

void test_v93_compile_fail_cross_module_private(void) {
    /* Source: tests/compile_fail/v3_cross_module_private.iron
     * Expected substring: tests/compile_fail/v3_cross_module_private.expected
     *
     * Two-file synthetic source via @file: markers. The lexer-side hook
     * (Plan 93-04 Task 1) re-tags subsequent tokens with the new filename,
     * so the resolver sees lib.iron and main.iron as distinct source files
     * for the cross-module check. */
    parse_and_resolve(
        "-- @file: lib.iron\n"
        "func helper() -> Int { return 42 }\n"
        "-- @file: main.iron\n"
        "func main() {\n"
        "    println(\"{helper()}\")\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error(IRON_ERR_CROSS_MODULE_PRIVATE),
        "expected E0320 IRON_ERR_CROSS_MODULE_PRIVATE for cross-module private ref");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("not visible from"),
        "expected E0320 'not visible from' substring (locks "
        "tests/compile_fail/v3_cross_module_private.expected)");
}

void test_v93_compile_fail_pub_init_in_private_object(void) {
    /* Source: tests/compile_fail/v3_pub_init_in_private_object.iron
     * Expected substring: tests/compile_fail/v3_pub_init_in_private_object.expected
     *
     * Plan 93-02 emits the refreshed message that names the enclosing
     * object. The fixture is intentionally minimal: `object Foo { pub
     * init() {} }`. */
    parse_and_resolve(
        "object Foo {\n"
        "    pub init() {}\n"
        "}\n"
    );
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("`pub init` is only valid inside a `pub object`"),
        "expected pub-init-in-private-object substring (locks "
        "tests/compile_fail/v3_pub_init_in_private_object.expected)");
}

void test_v93_compile_fail_pub_top_level_val(void) {
    /* Source: tests/compile_fail/v3_pub_top_level_val.iron
     * Expected substring: tests/compile_fail/v3_pub_top_level_val.expected
     *
     * Plan 93-01 rejects top-level `pub val` and points users at `pub func`
     * returning a constant. */
    parse_and_resolve("pub val GLOBAL: Int = 0\n");
    TEST_ASSERT_TRUE_MESSAGE(
        has_error_msg_substring("top-level `pub val`"),
        "expected top-level pub val rejection substring (locks "
        "tests/compile_fail/v3_pub_top_level_val.expected)");
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

    /* Phase 84 MUTTIER-03 Task 2: pure tier enforcement. */
    RUN_TEST(test_pure_writes_self_emits_E0244);
    RUN_TEST(test_pure_calls_println_emits_E0240);
    RUN_TEST(test_pure_calls_print_emits_E0240);
    RUN_TEST(test_pure_heap_alloc_is_ok);
    RUN_TEST(test_pure_writes_param_emits_E0243);
    RUN_TEST(test_pure_calls_non_pure_method_emits_E0242);
    RUN_TEST(test_pure_calls_pure_method_is_ok);
    RUN_TEST(test_pure_reads_mutable_global_emits_E0241);
    RUN_TEST(test_pure_writes_mutable_global_emits_E0241);
    RUN_TEST(test_pure_reads_val_global_is_ok);
    RUN_TEST(test_mutating_calls_on_val_binding_still_emits_E0235);
    RUN_TEST(test_readonly_callable_on_val_binding);

    /* Phase 85 INIT Plan 85-02 Task 1: definite-assignment + delegation + return-value. */
    RUN_TEST(test_init_read_before_assign_emits_E0246);
    RUN_TEST(test_init_all_fields_assigned_no_E0247);
    RUN_TEST(test_init_missing_field_emits_E0247);
    RUN_TEST(test_init_missing_on_branch_emits_E0247);
    RUN_TEST(test_init_assigned_on_both_branches_ok);
    RUN_TEST(test_init_val_double_assign_emits_E0248);
    RUN_TEST(test_init_var_double_assign_ok);
    RUN_TEST(test_init_method_on_partial_self_emits_E0249);
    RUN_TEST(test_init_method_on_full_self_ok);
    RUN_TEST(test_init_early_return_emits_E0250);
    RUN_TEST(test_init_bare_return_after_full_assign_ok);
    RUN_TEST(test_init_return_value_emits_E0252);
    RUN_TEST(test_init_delegation_to_anonymous_emits_E0251);
    RUN_TEST(test_init_delegation_to_named_emits_E0251);
    RUN_TEST(test_init_delegation_via_self_init_emits_E0251);
    RUN_TEST(test_method_call_Type_args_not_delegation);

    /* Phase 85 INIT Plan 85-02 Task 2: call-site dispatch + INIT-16 regression. */
    RUN_TEST(test_construct_dispatches_to_anonymous_init);
    RUN_TEST(test_construct_falls_back_to_v2_2_positional_when_no_init);
    RUN_TEST(test_construct_arg_count_checked_against_init_params);
    RUN_TEST(test_construct_arg_types_checked_against_init_params);
    RUN_TEST(test_named_init_dispatch);
    RUN_TEST(test_named_init_wrong_args);
    RUN_TEST(test_named_init_unknown_name_falls_through);
    RUN_TEST(test_method_body_can_call_Type_args);
    RUN_TEST(test_method_body_can_call_Type_name_args);

    /* Phase 86 Plan 86-02 Task 1: patch registry + primitive targets + E0254. */
    RUN_TEST(test_patch_registry_registers_user_object);
    RUN_TEST(test_patch_registry_registers_primitive);
    RUN_TEST(test_patch_unknown_target_emits_e0254);
    RUN_TEST(test_patch_multiple_patches_same_target);
    RUN_TEST(test_patch_primitive_names_allowlist);

    /* Phase 86 Plan 86-02 Task 2: collision scan + dispatch + regression locks. */
    RUN_TEST(test_patch_patch_collision);
    RUN_TEST(test_patch_object_collision);
    RUN_TEST(test_patch_collision_across_different_targets_no_error);
    RUN_TEST(test_patch_collision_different_names_no_error);
    RUN_TEST(test_patch_method_call_dispatches_through_registry_user);
    RUN_TEST(test_patch_method_call_dispatches_through_registry_primitive);
    RUN_TEST(test_patch_named_init_dispatches);
    RUN_TEST(test_patched_init_unassigned_field);
    RUN_TEST(test_patched_readonly_method_write_self_rejected);
    RUN_TEST(test_patched_pure_method_io_rejected);

    /* Phase 87 Plan 87-01 Task 2: IFACE-02 tier-strengthening (E0257). */
    RUN_TEST(test_iface_readonly_impl_default_mutating_rejected);
    RUN_TEST(test_iface_pure_impl_readonly_rejected);
    RUN_TEST(test_iface_pure_impl_default_rejected);
    RUN_TEST(test_iface_readonly_impl_readonly_accepted);
    RUN_TEST(test_iface_readonly_impl_pure_accepted);
    RUN_TEST(test_iface_pure_impl_pure_accepted);
    RUN_TEST(test_iface_default_impl_mutating_accepted);
    RUN_TEST(test_iface_default_impl_readonly_accepted);
    RUN_TEST(test_iface_default_body_inherited_no_mismatch);
    RUN_TEST(test_iface_tier_mismatch_E0257_locked_substring);

    /* Phase 87-02 SELF-01/02/03 + IFACE-05: Self type + Self-constructor resolution. */
    RUN_TEST(test_self_return_resolves_to_enclosing);
    RUN_TEST(test_self_named_init_resolves);
    RUN_TEST(test_self_outside_method_rejected);
    RUN_TEST(test_self_return_inside_init);
    RUN_TEST(test_self_in_interface_sig_bound_to_implementer);
    RUN_TEST(test_self_in_nested_method_resolves_correctly);

    /* Phase 87-02 PATCH-08: retroactive conformance scan + E0258. */
    RUN_TEST(test_patch_retroactive_conformance_full);
    RUN_TEST(test_patch_retroactive_missing_method_e0258);
    RUN_TEST(test_patch_retroactive_partial_coverage);
    RUN_TEST(test_patch_retroactive_with_default_body);
    RUN_TEST(test_patch_retroactive_tier_strengthening);
    RUN_TEST(test_classic_object_conformance_still_works);
    RUN_TEST(test_classic_object_missing_method_emits_e0258);

    /* Phase 93 VIS-01 Plan 93-01: top-level `pub` parser threading. */
    RUN_TEST(test_v93_pub_func_top_level_parses);
    RUN_TEST(test_v93_top_level_func_default_private);
    RUN_TEST(test_v93_pub_object_parses);
    RUN_TEST(test_v93_pub_enum_parses);
    RUN_TEST(test_v93_pub_patch_object_parses);
    RUN_TEST(test_v93_pub_private_combined_rejected);
    RUN_TEST(test_v93_top_level_pub_init_rejected);
    RUN_TEST(test_v93_top_level_pub_val_rejected);
    RUN_TEST(test_v93_pub_init_in_pub_object_accepted);
    RUN_TEST(test_v93_pub_init_in_private_object_rejected);
    RUN_TEST(test_v93_plain_init_in_pub_object_stays_private);

    /* Phase 93 Plan 93-03: resolver-side is_pub propagation + E0320
     * cross-module visibility check + stdlib carve-out. */
    RUN_TEST(test_v93_e0320_macro_registered);
    RUN_TEST(test_v93_sym_is_pub_propagates_func);
    RUN_TEST(test_v93_sym_is_pub_default_false_func);
    RUN_TEST(test_v93_sym_is_pub_propagates_enum_and_variants);
    RUN_TEST(test_v93_sym_is_pub_default_false_enum_variants);
    RUN_TEST(test_v93_sym_is_pub_propagates_object);
    RUN_TEST(test_v93_e0320_cross_module_private_rejected);
    RUN_TEST(test_v93_e0320_cross_module_pub_accepted);
    RUN_TEST(test_v93_e0320_same_file_private_accepted);
    RUN_TEST(test_v93_e0320_audit_note_rate_limited);

    /* Phase 93 Plan 04 Task 3: compile_fail fixture substring locks. Pair
     * one unit test per tests/compile_fail/v3_*.iron + .expected fixture so
     * the substring lock gates CI (tests/compile_fail/ is not auto-discovered
     * by run_tests.sh). */
    RUN_TEST(test_v93_compile_fail_cross_module_private);
    RUN_TEST(test_v93_compile_fail_pub_init_in_private_object);
    RUN_TEST(test_v93_compile_fail_pub_top_level_val);

    return UNITY_END();
}
