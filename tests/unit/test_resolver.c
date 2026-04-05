/* test_resolver.c — Unity tests for the Iron two-pass name resolver.
 *
 * Tests cover:
 *   - basic function/variable resolution
 *   - forward references (method before object decl)
 *   - undefined variable => E0200
 *   - duplicate declaration => E0201
 *   - nested scope lookup
 *   - block scope isolation
 *   - self inside method resolves; self outside => E0210
 *   - super in child method resolves; super in non-extending type => E0211
 *   - unresolved method type_name => E0200
 *   - enum variant registration
 *   - import declaration recorded (no error)
 */

#include "unity.h"
#include "analyzer/resolve.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <string.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 256);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Parse helper ────────────────────────────────────────────────────────── */

static Iron_Program *parse_program(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &g_arena, &g_diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &g_arena, &g_diags);
    Iron_Node   *root = iron_parse(&p);
    return (Iron_Program *)root;
}

/* ── Helper: find an ident node named `name` in program by walking decls ── */

/* Recursively searches for an Iron_Ident node with the given name.
 * Returns the first match, or NULL if not found. */
static Iron_Ident *find_ident(Iron_Node *node, const char *name);

static Iron_Ident *find_ident_in_array(Iron_Node **nodes, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        Iron_Ident *found = find_ident(nodes[i], name);
        if (found) return found;
    }
    return NULL;
}

static Iron_Ident *find_ident(Iron_Node *node, const char *name) {
    if (!node) return NULL;
    switch (node->kind) {
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;
            if (strcmp(id->name, name) == 0) return id;
            return NULL;
        }
        case IRON_NODE_PROGRAM: {
            Iron_Program *p = (Iron_Program *)node;
            return find_ident_in_array(p->decls, p->decl_count, name);
        }
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)node;
            return find_ident(fd->body, name);
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *md = (Iron_MethodDecl *)node;
            return find_ident(md->body, name);
        }
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            return find_ident_in_array(b->stmts, b->stmt_count, name);
        }
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
            return find_ident(vd->init, name);
        }
        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            return find_ident(vd->init, name);
        }
        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            return find_ident(rs->value, name);
        }
        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            Iron_Ident *found = find_ident(be->left, name);
            if (found) return found;
            return find_ident(be->right, name);
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            Iron_Ident *found = find_ident(is->condition, name);
            if (found) return found;
            found = find_ident(is->body, name);
            if (found) return found;
            return find_ident(is->else_body, name);
        }
        default:
            return NULL;
    }
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* Test 1: func main resolved as SYM_FUNCTION; val x as SYM_VARIABLE */
void test_resolve_func_and_val(void) {
    const char *src =
        "func main() {\n"
        "  val x = 42\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* main should be in global scope as SYM_FUNCTION */
    Iron_Symbol *main_sym = iron_scope_lookup(global, "main");
    TEST_ASSERT_NOT_NULL(main_sym);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_FUNCTION, main_sym->sym_kind);
}

/* Test 2: val x = 42; val y = x — y's init ident resolves to x's symbol */
void test_resolve_val_reference(void) {
    const char *src =
        "func foo() {\n"
        "  val x = 42\n"
        "  val y = x\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find the ident 'x' that is the init of y */
    Iron_FuncDecl *fd = (Iron_FuncDecl *)prog->decls[0];
    Iron_Block    *body = (Iron_Block *)fd->body;
    /* body->stmts[0] = val x, body->stmts[1] = val y */
    Iron_ValDecl  *y_decl = (Iron_ValDecl *)body->stmts[1];
    Iron_Ident    *x_ref  = (Iron_Ident *)y_decl->init;
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, x_ref->kind);
    TEST_ASSERT_NOT_NULL(x_ref->resolved_sym);
    TEST_ASSERT_EQUAL_STRING("x", x_ref->resolved_sym->name);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_VARIABLE, x_ref->resolved_sym->sym_kind);
}

/* Test 3: undefined variable => IRON_ERR_UNDEFINED_VAR (E0200) */
void test_resolve_undefined_var(void) {
    const char *src =
        "func foo() {\n"
        "  val y = z\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_EQUAL_INT(1, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_UNDEFINED_VAR, g_diags.items[0].code);
}

/* Test 4: duplicate declaration => IRON_ERR_DUPLICATE_DECL (E0201) */
void test_resolve_duplicate_decl(void) {
    const char *src =
        "func foo() {\n"
        "  val x = 1\n"
        "  val x = 2\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
    /* Check at least one IRON_ERR_DUPLICATE_DECL was emitted */
    bool found_dup = false;
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == IRON_ERR_DUPLICATE_DECL) {
            found_dup = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(found_dup);
}

/* Test 5: forward reference — method decl before object decl */
void test_resolve_forward_reference_method_before_object(void) {
    const char *src =
        "func Player.update() {\n"
        "  val x = 1\n"
        "}\n"
        "object Player {\n"
        "  var hp: Int\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Player should be in global scope as SYM_TYPE */
    Iron_Symbol *player_sym = iron_scope_lookup(global, "Player");
    TEST_ASSERT_NOT_NULL(player_sym);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_TYPE, player_sym->sym_kind);

    /* MethodDecl owner_sym should be set to Player's symbol */
    Iron_MethodDecl *md = (Iron_MethodDecl *)prog->decls[0];
    TEST_ASSERT_EQUAL_INT(IRON_NODE_METHOD_DECL, md->kind);
    TEST_ASSERT_NOT_NULL(md->owner_sym);
    TEST_ASSERT_EQUAL_STRING("Player", md->owner_sym->name);
}

/* Test 6: nested scope — inner block can see outer variable */
void test_resolve_nested_scope_lookup(void) {
    const char *src =
        "func foo() {\n"
        "  val x = 1\n"
        "  if true {\n"
        "    val y = x\n"
        "  }\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Find the 'x' ident inside the if-body */
    Iron_FuncDecl *fd    = (Iron_FuncDecl *)prog->decls[0];
    Iron_Block    *body  = (Iron_Block *)fd->body;
    /* stmts[0] = val x, stmts[1] = if stmt */
    Iron_IfStmt   *ifs   = (Iron_IfStmt *)body->stmts[1];
    Iron_Block    *ib    = (Iron_Block *)ifs->body;
    /* ib->stmts[0] = val y = x */
    Iron_ValDecl  *y_d   = (Iron_ValDecl *)ib->stmts[0];
    Iron_Ident    *x_ref = (Iron_Ident *)y_d->init;
    TEST_ASSERT_EQUAL_INT(IRON_NODE_IDENT, x_ref->kind);
    TEST_ASSERT_NOT_NULL(x_ref->resolved_sym);
    TEST_ASSERT_EQUAL_STRING("x", x_ref->resolved_sym->name);
}

/* Test 7: block scope isolation — variable from if-block not visible outside */
void test_resolve_block_scope_isolation(void) {
    const char *src =
        "func foo() {\n"
        "  if true {\n"
        "    val x = 1\n"
        "  }\n"
        "  val y = x\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    /* x is only defined in the if-block; y = x should produce E0200 */
    TEST_ASSERT_EQUAL_INT(1, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_UNDEFINED_VAR, g_diags.items[0].code);
}

/* Test 8: self inside method resolves without error */
void test_resolve_self_inside_method(void) {
    const char *src =
        "object Player {\n"
        "  var hp: Int\n"
        "}\n"
        "func Player.tick() {\n"
        "  val s = self\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* self ident should have resolved_sym set */
    Iron_MethodDecl *md  = (Iron_MethodDecl *)prog->decls[1];
    Iron_Block      *body = (Iron_Block *)md->body;
    Iron_ValDecl    *vd  = (Iron_ValDecl *)body->stmts[0];
    Iron_Ident      *sid = (Iron_Ident *)vd->init;
    TEST_ASSERT_NOT_NULL(sid->resolved_sym);
    TEST_ASSERT_EQUAL_STRING("self", sid->resolved_sym->name);
}

/* Test 9: self outside method => E0210 */
void test_resolve_self_outside_method(void) {
    const char *src =
        "func notMethod() {\n"
        "  val s = self\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_EQUAL_INT(1, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_SELF_OUTSIDE_METHOD, g_diags.items[0].code);
}

/* Test 10: super in child method resolves */
void test_resolve_super_in_child_method(void) {
    const char *src =
        "object Animal {\n"
        "  var hp: Int\n"
        "}\n"
        "object Dog extends Animal {\n"
        "  var name: String\n"
        "}\n"
        "func Dog.bark() {\n"
        "  val p = super\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 11: super in non-extending type => E0211 */
void test_resolve_super_no_parent(void) {
    const char *src =
        "object Solo {\n"
        "  var x: Int\n"
        "}\n"
        "func Solo.action() {\n"
        "  val p = super\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_EQUAL_INT(1, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(IRON_ERR_SUPER_NO_PARENT, g_diags.items[0].code);
}

/* Test 12: method with unresolved type_name produces error */
void test_resolve_method_unresolved_type(void) {
    const char *src =
        "func UnknownType.bar() {\n"
        "  val x = 1\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_GREATER_THAN(0, g_diags.error_count);
}

/* Test 13: enum variants accessible in global scope */
void test_resolve_enum_variants_registered(void) {
    const char *src =
        "enum Color {\n"
        "  Red,\n"
        "  Green,\n"
        "  Blue\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* enum itself */
    Iron_Symbol *color = iron_scope_lookup(global, "Color");
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_ENUM, color->sym_kind);

    /* variants registered in global scope */
    Iron_Symbol *red = iron_scope_lookup(global, "Red");
    TEST_ASSERT_NOT_NULL(red);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_ENUM_VARIANT, red->sym_kind);
}

/* Test 14: import declaration is recorded without error */
void test_resolve_import_no_error(void) {
    const char *src = "import std.math\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 15: interface registered in global scope */
void test_resolve_interface_registered(void) {
    const char *src =
        "interface Drawable {\n"
        "  func draw() -> void\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);

    Iron_Symbol *sym = iron_scope_lookup(global, "Drawable");
    TEST_ASSERT_NOT_NULL(sym);
    TEST_ASSERT_EQUAL_INT(IRON_SYM_INTERFACE, sym->sym_kind);
}

/* ── ADT resolver tests ──────────────────────────────────────────────────── */

static bool has_error_code(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

/* Test 16: pattern binding 'r' introduced into arm scope without error */
void test_resolve_adt_pattern_binding(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "}\n"
        "func main() {\n"
        "  match 1 {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = 1\n"
        "    }\n"
        "    else -> {}\n"
        "  }\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 17: wildcard _ in pattern does not introduce a binding (no error) */
void test_resolve_adt_wildcard_no_binding(void) {
    const char *src =
        "enum Shape {\n"
        "  Rect(Float, Float),\n"
        "}\n"
        "func main() {\n"
        "  match 1 {\n"
        "    Shape.Rect(_, h) -> {\n"
        "      val x = 1\n"
        "    }\n"
        "    else -> {}\n"
        "  }\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 18: pattern binding same name as outer variable => IRON_ERR_BINDING_SHADOWS (227) */
void test_resolve_adt_binding_shadows_error(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "}\n"
        "func main() {\n"
        "  val r = 1\n"
        "  match 1 {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = 1\n"
        "    }\n"
        "    else -> {}\n"
        "  }\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_TRUE(has_error_code(227));
}

/* Test 19: pattern references non-existent variant => IRON_ERR_UNKNOWN_VARIANT (228) */
void test_resolve_adt_unknown_variant_error(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "}\n"
        "func main() {\n"
        "  match 1 {\n"
        "    Shape.Triangle(x) -> {\n"
        "      val y = 1\n"
        "    }\n"
        "    else -> {}\n"
        "  }\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_TRUE(has_error_code(228));
}

/* Test 20: enum construction Shape.Circle(1.0) resolves without error */
void test_resolve_adt_enum_construct_ok(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "}\n"
        "func main() {\n"
        "  val s = Shape.Circle(1.0)\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* Test 21: same binding name 'r' in two arms is fine (each arm has its own scope) */
void test_resolve_adt_same_binding_across_arms(void) {
    const char *src =
        "enum Shape {\n"
        "  Circle(Float),\n"
        "  Square(Float),\n"
        "}\n"
        "func main() {\n"
        "  match 1 {\n"
        "    Shape.Circle(r) -> {\n"
        "      val x = 1\n"
        "    }\n"
        "    Shape.Square(r) -> {\n"
        "      val y = 1\n"
        "    }\n"
        "  }\n"
        "}\n";
    Iron_Program *prog = parse_program(src);
    Iron_Scope *global = iron_resolve(prog, &g_arena, &g_diags);
    TEST_ASSERT_NOT_NULL(global);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_resolve_func_and_val);
    RUN_TEST(test_resolve_val_reference);
    RUN_TEST(test_resolve_undefined_var);
    RUN_TEST(test_resolve_duplicate_decl);
    RUN_TEST(test_resolve_forward_reference_method_before_object);
    RUN_TEST(test_resolve_nested_scope_lookup);
    RUN_TEST(test_resolve_block_scope_isolation);
    RUN_TEST(test_resolve_self_inside_method);
    RUN_TEST(test_resolve_self_outside_method);
    RUN_TEST(test_resolve_super_in_child_method);
    RUN_TEST(test_resolve_super_no_parent);
    RUN_TEST(test_resolve_method_unresolved_type);
    RUN_TEST(test_resolve_enum_variants_registered);
    RUN_TEST(test_resolve_import_no_error);
    RUN_TEST(test_resolve_interface_registered);
    RUN_TEST(test_resolve_adt_pattern_binding);
    RUN_TEST(test_resolve_adt_wildcard_no_binding);
    RUN_TEST(test_resolve_adt_binding_shadows_error);
    RUN_TEST(test_resolve_adt_unknown_variant_error);
    RUN_TEST(test_resolve_adt_enum_construct_ok);
    RUN_TEST(test_resolve_adt_same_binding_across_arms);

    return UNITY_END();
}
