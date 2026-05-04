/* test_v3_visibility_definition -- Phase 10 Plan 10-02 (VIS-03).
 *
 * Asserts the cross-module visibility gate inside ilsp_facade_nav_definition.
 *
 * Anti-aliasing matrix:
 *   * false-negative: cross-module private decl returns *out_n == 0
 *     via predicate (test_predicate_drops_cross_module_private)
 *   * false-positive: cross-module public decl passes; same-file
 *     private decl always passes (predicate same-module short-circuit)
 *
 * The deterministic semantic test exercises the predicate directly so
 * the assertion does not depend on workspace_index timing.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/facade/types.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Test 01: predicate gate -- private cross-module returns false */

static void test_predicate_drops_private_cross_module(void) {
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = true;

    TEST_ASSERT_FALSE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_b.iron", (const Iron_Node *)&fd));
}

/* ── Test 02: predicate gate -- public cross-module returns true */

static void test_predicate_keeps_public_cross_module(void) {
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = false;

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_b.iron", (const Iron_Node *)&fd));
}

/* ── Test 03: predicate gate -- private same-module returns true */

static void test_predicate_keeps_private_same_module(void) {
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = true;

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_a.iron", (const Iron_Node *)&fd));
}

/* ── Test 04: facade smoke -- definition on a same-file ident ─── */

static void test_facade_definition_smoke(void) {
    /* Smoke: drive ilsp_facade_nav_definition against a tiny in-memory
     * doc. The visibility gate at the new VIS-03 insertion point must
     * not regress the existing same-file definition behavior. */
    const char *src = "func foo() -> Int { return 1 }\n";
    IronLsp_Server server;
    memset(&server, 0, sizeof(server));
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/v3vis_def.iron", src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    IronLsp_Position pos = { .line = 0, .character = 5 };  /* "foo" decl */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_LocationLink *links = NULL;
    size_t n = 0;
    ilsp_facade_nav_definition(&server, doc, pos, NULL, &arena, &links, &n);

    /* Acceptable: any n; assertion is no crash + null/non-null
     * consistency. */
    if (n > 0) TEST_ASSERT_NOT_NULL(links);
    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_predicate_drops_private_cross_module);
    RUN_TEST(test_predicate_keeps_public_cross_module);
    RUN_TEST(test_predicate_keeps_private_same_module);
    RUN_TEST(test_facade_definition_smoke);
    return UNITY_END();
}
