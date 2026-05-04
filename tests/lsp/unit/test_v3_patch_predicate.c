/* test_v3_patch_predicate -- Phase 11 Plan 11-01 (PATCH-01, D-15).
 *
 * Stateless predicate verification for the 3 patch_lookup helpers:
 *   - ilsp_patch_enclosing_for_method
 *   - ilsp_patch_target_matches_interface
 *   - ilsp_patch_for_each_method (registry-backed; uses the
 *     v3_patch/patch_implementation_user_object.iron fixture)
 *
 * Per src/parser/ast.h Iron_ObjectDecl / Iron_MethodDecl /
 * Iron_InterfaceDecl are anonymous typedef structs whose first two
 * fields are { Iron_Span span; Iron_NodeKind kind; }. They do NOT
 * embed an Iron_Node `base` member; the structural-subtyping prefix
 * is laid out inline. Tests therefore set `kind` directly on the
 * stack-allocated concrete decl and cast to const Iron_Node * at
 * call sites. */

#include "unity.h"

#include "lsp/facade/nav/patch_lookup.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Test-only stubs for workspace_index symbols ────────────────────
 *
 * patch_lookup.c references ilsp_workspace_index_{snapshot_paths,
 * lookup,analyze_lazy} for its cross-file walk. The predicate test
 * always passes wi=NULL so the calls are dead, but the linker still
 * needs the symbols. Stubbing here keeps the predicate test on a
 * minimal link set (no workspace_index.c + transitive deps).  When
 * wi != NULL, these stubs would short-circuit safely (return NULL/0)
 * but the predicate tests never exercise that path. */
struct IronLsp_WorkspaceIndex;
typedef struct IronLsp_IndexEntry IronLsp_IndexEntry;
struct Iron_Program_;
char **ilsp_workspace_index_snapshot_paths(
    struct IronLsp_WorkspaceIndex *wi, size_t *out_n) {
    (void)wi; if (out_n) *out_n = 0; return NULL;
}
IronLsp_IndexEntry *ilsp_workspace_index_lookup(
    struct IronLsp_WorkspaceIndex *wi, const char *p) {
    (void)wi; (void)p; return NULL;
}
struct Iron_Program *ilsp_workspace_index_analyze_lazy(
    struct IronLsp_WorkspaceIndex *wi, IronLsp_IndexEntry *e,
    _Atomic bool *cancel) {
    (void)wi; (void)e; (void)cancel; return NULL;
}

void setUp(void)    {}
void tearDown(void) {}

/* ── Fixture loader (mirrors test_v3_tier_completion.c) ──────────── */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static const char *fixture_path(char *buf, size_t cap, const char *name) {
    snprintf(buf, cap, "../tests/lsp/unit/v3_patch/%s", name);
    FILE *f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#ifdef IRON_SOURCE_TREE_ROOT
    snprintf(buf, cap, "%s/tests/lsp/unit/v3_patch/%s",
             IRON_SOURCE_TREE_ROOT, name);
    f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#endif
    return NULL;
}

static Iron_Program *parse_source(const char *src, Iron_Arena *arena,
                                    Iron_DiagList *diags) {
    Iron_Lexer lx = iron_lexer_create(src, "<test>", arena, diags);
    Iron_Token *toks = iron_lex_all(&lx);
    int tok_count = (int)arrlen(toks);
    Iron_Parser p = iron_parser_create(toks, tok_count, src, "<test>",
                                         arena, diags);
    Iron_Node *prog = iron_parse(&p);
    arrfree(toks);
    return (Iron_Program *)prog;
}

/* ── Test 1: NULL inputs ─────────────────────────────────────────── */

static void test_null_inputs(void) {
    TEST_ASSERT_NULL(ilsp_patch_enclosing_for_method(NULL, NULL, NULL));
    TEST_ASSERT_FALSE(ilsp_patch_target_matches_interface(NULL, NULL, NULL));
    /* visit=NULL & target=NULL each individually short-circuit to 0. */
    TEST_ASSERT_EQUAL_size_t(0,
        ilsp_patch_for_each_method(NULL, NULL, NULL, NULL, NULL, NULL, NULL));
}

/* ── Test 2: enclosing same-program hit ──────────────────────────── */

static void test_enclosing_same_program_hit(void) {
    Iron_ObjectDecl od; memset(&od, 0, sizeof(od));
    od.kind             = IRON_NODE_OBJECT_DECL;
    od.is_patch         = true;
    od.target_type_name = "Int";
    od.name             = "Int";

    Iron_MethodDecl md; memset(&md, 0, sizeof(md));
    md.kind        = IRON_NODE_METHOD_DECL;
    md.type_name   = "Int";
    md.method_name = "double";

    Iron_Node *decls[1] = { (Iron_Node *)&od };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 1;

    TEST_ASSERT_EQUAL_PTR(&od,
        ilsp_patch_enclosing_for_method(&prog, &md, NULL));
}

/* ── Test 3: enclosing target mismatch ───────────────────────────── */

static void test_enclosing_target_mismatch(void) {
    Iron_ObjectDecl od; memset(&od, 0, sizeof(od));
    od.kind             = IRON_NODE_OBJECT_DECL;
    od.is_patch         = true;
    od.target_type_name = "Int";

    Iron_MethodDecl md; memset(&md, 0, sizeof(md));
    md.kind      = IRON_NODE_METHOD_DECL;
    md.type_name = "Float";  /* different type */

    Iron_Node *decls[1] = { (Iron_Node *)&od };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 1;

    TEST_ASSERT_NULL(ilsp_patch_enclosing_for_method(&prog, &md, NULL));
}

/* ── Test 4: declaration-order first-match wins ──────────────────── */

static void test_enclosing_first_match_wins(void) {
    Iron_ObjectDecl od_a; memset(&od_a, 0, sizeof(od_a));
    od_a.kind             = IRON_NODE_OBJECT_DECL;
    od_a.is_patch         = true;
    od_a.target_type_name = "Int";

    Iron_ObjectDecl od_b; memset(&od_b, 0, sizeof(od_b));
    od_b.kind             = IRON_NODE_OBJECT_DECL;
    od_b.is_patch         = true;
    od_b.target_type_name = "Int";

    Iron_MethodDecl md; memset(&md, 0, sizeof(md));
    md.kind      = IRON_NODE_METHOD_DECL;
    md.type_name = "Int";

    Iron_Node *decls[2] = { (Iron_Node *)&od_a, (Iron_Node *)&od_b };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 2;

    /* od_a comes first in decls[]; helper returns first match. */
    TEST_ASSERT_EQUAL_PTR(&od_a,
        ilsp_patch_enclosing_for_method(&prog, &md, NULL));
}

/* ── Test 5: target_matches_interface user-object positive ───────── */

static void test_target_matches_user_object_with_implements(void) {
    /* User object MyGreeter implements Greeter. */
    const char *impls[] = { "Greeter" };
    Iron_ObjectDecl mygreeter; memset(&mygreeter, 0, sizeof(mygreeter));
    mygreeter.kind              = IRON_NODE_OBJECT_DECL;
    mygreeter.is_patch          = false;
    mygreeter.name              = "MyGreeter";
    mygreeter.implements_names  = impls;
    mygreeter.implements_count  = 1;

    /* Patch targets MyGreeter. */
    Iron_ObjectDecl patch; memset(&patch, 0, sizeof(patch));
    patch.kind             = IRON_NODE_OBJECT_DECL;
    patch.is_patch         = true;
    patch.target_type_name = "MyGreeter";

    /* Interface Greeter. */
    Iron_InterfaceDecl ifc; memset(&ifc, 0, sizeof(ifc));
    ifc.kind = IRON_NODE_INTERFACE_DECL;
    ifc.name = "Greeter";

    Iron_Node *decls[2] = { (Iron_Node *)&mygreeter, (Iron_Node *)&patch };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 2;

    TEST_ASSERT_TRUE(
        ilsp_patch_target_matches_interface(&patch, &ifc, &prog));
}

/* ── Test 6: target_matches_interface primitive returns false ───── */

static void test_target_matches_primitive_returns_false(void) {
    /* Patch targets primitive Int — no Iron_ObjectDecl exists for
     * primitives in program->decls, so the find_user_object_by_name
     * lookup misses. */
    Iron_ObjectDecl patch; memset(&patch, 0, sizeof(patch));
    patch.kind             = IRON_NODE_OBJECT_DECL;
    patch.is_patch         = true;
    patch.target_type_name = "Int";

    Iron_InterfaceDecl ifc; memset(&ifc, 0, sizeof(ifc));
    ifc.kind = IRON_NODE_INTERFACE_DECL;
    ifc.name = "Greeter";

    Iron_Node *decls[1] = { (Iron_Node *)&patch };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 1;

    TEST_ASSERT_FALSE(
        ilsp_patch_target_matches_interface(&patch, &ifc, &prog));
}

/* ── Test 7: target_matches_interface name mismatch ─────────────── */

static void test_target_matches_iface_name_mismatch(void) {
    const char *impls[] = { "OtherIface" };  /* not Greeter */
    Iron_ObjectDecl mygreeter; memset(&mygreeter, 0, sizeof(mygreeter));
    mygreeter.kind              = IRON_NODE_OBJECT_DECL;
    mygreeter.is_patch          = false;
    mygreeter.name              = "MyGreeter";
    mygreeter.implements_names  = impls;
    mygreeter.implements_count  = 1;

    Iron_ObjectDecl patch; memset(&patch, 0, sizeof(patch));
    patch.kind             = IRON_NODE_OBJECT_DECL;
    patch.is_patch         = true;
    patch.target_type_name = "MyGreeter";

    Iron_InterfaceDecl ifc; memset(&ifc, 0, sizeof(ifc));
    ifc.kind = IRON_NODE_INTERFACE_DECL;
    ifc.name = "Greeter";  /* not in implements_names */

    Iron_Node *decls[2] = { (Iron_Node *)&mygreeter, (Iron_Node *)&patch };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 2;

    TEST_ASSERT_FALSE(
        ilsp_patch_target_matches_interface(&patch, &ifc, &prog));
}

/* ── Test 8: for_each_method visits visitor (real fixture) ─────── */

static int g_visit_count;

static bool count_visitor(Iron_MethodDecl *mm, Iron_ObjectDecl *od, void *ud) {
    (void)mm; (void)od; (void)ud;
    g_visit_count++;
    return true;
}

static void test_for_each_method_visits_visitor(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf),
                                     "patch_implementation_user_object.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path,
        "fixture patch_implementation_user_object.iron not found");
    char *src = load_file(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(src, "load_file returned NULL");

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog = parse_source(src, &arena, &diags);
    TEST_ASSERT_NOT_NULL(prog);

    /* The fixture has `patch object MyGreeter { func extra() ... }` —
     * exactly one patched method on MyGreeter. */
    g_visit_count = 0;
    size_t visited = ilsp_patch_for_each_method(
        prog, /*wi=*/NULL, "MyGreeter",
        /*requester_canonical=*/"<test>",
        count_visitor, /*ud=*/NULL, /*cancel=*/NULL);

    TEST_ASSERT_EQUAL_size_t(1, visited);
    TEST_ASSERT_EQUAL_INT(1, g_visit_count);

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(src);
}

/* ── Test 9: determinism gate (1000 sequential calls) ───────────── */

static void test_determinism_gate(void) {
    Iron_ObjectDecl od; memset(&od, 0, sizeof(od));
    od.kind             = IRON_NODE_OBJECT_DECL;
    od.is_patch         = true;
    od.target_type_name = "Int";

    Iron_MethodDecl md; memset(&md, 0, sizeof(md));
    md.kind      = IRON_NODE_METHOD_DECL;
    md.type_name = "Int";

    Iron_Node *decls[1] = { (Iron_Node *)&od };
    Iron_Program prog; memset(&prog, 0, sizeof(prog));
    prog.kind       = IRON_NODE_PROGRAM;
    prog.decls      = decls;
    prog.decl_count = 1;

    Iron_ObjectDecl *first =
        ilsp_patch_enclosing_for_method(&prog, &md, NULL);
    TEST_ASSERT_NOT_NULL(first);

    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_EQUAL_PTR(first,
            ilsp_patch_enclosing_for_method(&prog, &md, NULL));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_null_inputs);
    RUN_TEST(test_enclosing_same_program_hit);
    RUN_TEST(test_enclosing_target_mismatch);
    RUN_TEST(test_enclosing_first_match_wins);
    RUN_TEST(test_target_matches_user_object_with_implements);
    RUN_TEST(test_target_matches_primitive_returns_false);
    RUN_TEST(test_target_matches_iface_name_mismatch);
    RUN_TEST(test_for_each_method_visits_visitor);
    RUN_TEST(test_determinism_gate);
    return UNITY_END();
}
