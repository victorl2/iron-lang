/* test_v3_visibility_predicate -- Phase 10 Plan 10-01 (D-02, D-08).
 *
 * Stateless predicate verification: every input combination yields a
 * deterministic bool. Includes:
 *   - NULL safety (defensive default-allow on empty paths)
 *   - same-module short-circuit (decl_canonical == requester_canonical)
 *   - same-string different-pointer same-module
 *   - stdlib carve-out (D-08): "stdlib://..." -> true regardless
 *   - is_pub field returns true (D-01 positive bit)
 *   - is_private func/method returns false cross-module (D-01 v2 inverse)
 *   - 1000-iteration determinism gate (Validation § Determinism Gate)
 *
 * Per src/parser/ast.h Iron_Field / Iron_FuncDecl / Iron_MethodDecl /
 * Iron_ObjectDecl are anonymous typedef structs whose first two fields
 * are { Iron_Span span; Iron_NodeKind kind; }. They do NOT embed an
 * Iron_Node `base` member; the structural-subtyping prefix is laid out
 * inline. Tests therefore set `kind` directly on the stack-allocated
 * concrete decl and cast to const Iron_Node * at the call site. */

#include "unity.h"

#include "lsp/facade/nav/visibility.h"
#include "parser/ast.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_null_inputs_return_false(void) {
    /* ilsp_vis_is_public(NULL) -> false (defensive default). */
    TEST_ASSERT_FALSE(ilsp_vis_is_public(NULL));
    /* ilsp_vis_can_see(NULL, NULL, NULL) -> true per D-04 / Pitfall 5
     * (open-doc-without-canonical-path treated as same-module). */
    TEST_ASSERT_TRUE(ilsp_vis_can_see(NULL, NULL, NULL));
    /* Half-NULL also default-allow -- both args MUST be set for the
     * cross-module gate to fire. */
    TEST_ASSERT_TRUE(ilsp_vis_can_see("/abs/foo.iron", NULL, NULL));
    TEST_ASSERT_TRUE(ilsp_vis_can_see(NULL, "/abs/foo.iron", NULL));
}

static void test_same_module_short_circuit(void) {
    const char *p = "/abs/foo.iron";
    /* Same pointer (arena-interned fast path). */
    TEST_ASSERT_TRUE(ilsp_vis_can_see(p, p, NULL));
    /* Same string, different pointer (must fall through to strcmp). */
    char copy[64]; strcpy(copy, p);
    TEST_ASSERT_TRUE(ilsp_vis_can_see(p, copy, NULL));
}

static void test_stdlib_carveout(void) {
    /* D-08: stdlib symbols always visible until Phase 14 MIG flips. */
    Iron_FuncDecl fd; memset(&fd, 0, sizeof(fd));
    fd.kind       = IRON_NODE_FUNC_DECL;
    fd.is_private = true;
    TEST_ASSERT_TRUE(ilsp_vis_can_see("stdlib://math",
                                       "/abs/user.iron",
                                       (const Iron_Node *)&fd));
}

static void test_pub_field_visible_cross_module(void) {
    Iron_Field f; memset(&f, 0, sizeof(f));
    f.kind   = IRON_NODE_FIELD;
    f.is_pub = true;
    TEST_ASSERT_TRUE(ilsp_vis_is_public((const Iron_Node *)&f));
    TEST_ASSERT_TRUE(ilsp_vis_can_see("/abs/decl.iron",
                                       "/abs/req.iron",
                                       (const Iron_Node *)&f));
}

static void test_private_field_invisible_cross_module(void) {
    Iron_Field f; memset(&f, 0, sizeof(f));
    f.kind   = IRON_NODE_FIELD;
    f.is_pub = false;
    TEST_ASSERT_FALSE(ilsp_vis_is_public((const Iron_Node *)&f));
    TEST_ASSERT_FALSE(ilsp_vis_can_see("/abs/decl.iron",
                                        "/abs/req.iron",
                                        (const Iron_Node *)&f));
}

static void test_private_func_invisible_cross_module(void) {
    Iron_FuncDecl fd; memset(&fd, 0, sizeof(fd));
    fd.kind       = IRON_NODE_FUNC_DECL;
    fd.is_private = true;
    TEST_ASSERT_FALSE(ilsp_vis_is_public((const Iron_Node *)&fd));
    TEST_ASSERT_FALSE(ilsp_vis_can_see("/abs/decl.iron",
                                        "/abs/req.iron",
                                        (const Iron_Node *)&fd));
}

static void test_private_method_invisible_cross_module(void) {
    Iron_MethodDecl md; memset(&md, 0, sizeof(md));
    md.kind       = IRON_NODE_METHOD_DECL;
    md.is_private = true;
    TEST_ASSERT_FALSE(ilsp_vis_is_public((const Iron_Node *)&md));
    TEST_ASSERT_FALSE(ilsp_vis_can_see("/abs/decl.iron",
                                        "/abs/req.iron",
                                        (const Iron_Node *)&md));
}

static void test_pub_func_visible_cross_module(void) {
    Iron_FuncDecl fd; memset(&fd, 0, sizeof(fd));
    fd.kind       = IRON_NODE_FUNC_DECL;
    fd.is_private = false;
    TEST_ASSERT_TRUE(ilsp_vis_is_public((const Iron_Node *)&fd));
    TEST_ASSERT_TRUE(ilsp_vis_can_see("/abs/decl.iron",
                                       "/abs/req.iron",
                                       (const Iron_Node *)&fd));
}

static void test_object_decl_default_true(void) {
    /* RESEARCH Conflict 3: ObjectDecl has no is_private; default-true. */
    Iron_ObjectDecl od; memset(&od, 0, sizeof(od));
    od.kind = IRON_NODE_OBJECT_DECL;
    TEST_ASSERT_TRUE(ilsp_vis_is_public((const Iron_Node *)&od));
}

static void test_determinism_gate(void) {
    /* Validation § Determinism Gate: 1000 iterations -> identical. */
    Iron_FuncDecl fd; memset(&fd, 0, sizeof(fd));
    fd.kind       = IRON_NODE_FUNC_DECL;
    fd.is_private = false;
    bool first = ilsp_vis_can_see("/a.iron", "/b.iron", (const Iron_Node *)&fd);
    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_EQUAL(first,
            ilsp_vis_can_see("/a.iron", "/b.iron", (const Iron_Node *)&fd));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_null_inputs_return_false);
    RUN_TEST(test_same_module_short_circuit);
    RUN_TEST(test_stdlib_carveout);
    RUN_TEST(test_pub_field_visible_cross_module);
    RUN_TEST(test_private_field_invisible_cross_module);
    RUN_TEST(test_private_func_invisible_cross_module);
    RUN_TEST(test_private_method_invisible_cross_module);
    RUN_TEST(test_pub_func_visible_cross_module);
    RUN_TEST(test_object_decl_default_true);
    RUN_TEST(test_determinism_gate);
    return UNITY_END();
}
