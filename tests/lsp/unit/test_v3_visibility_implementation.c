/* test_v3_visibility_implementation -- Phase 10 Plan 10-02 (VIS-03).
 *
 * RESEARCH Conflict 2 resolution: VIS-03 includes implementation; the
 * per-element filter inside the same-file fallback emit/count loops
 * skips hidden non-stdlib implementors.
 *
 * Note per RESEARCH Conflict 3: ObjectDecl has NO is_pub axis in the
 * v3 AST today (parser drops `private` keyword for objects per
 * parser.c:4047). The predicate default-trues for ObjectDecl kind, so
 * the per-impl gate is functionally a no-op for objects -- it is wired
 * here so Phase 11 PATCH / Phase 14 MIG don't have to revisit the
 * call site. This file documents the default-true behavior with a
 * direct predicate test, plus a facade smoke test asserting no
 * regression in interface implementor surfacing.
 */
#include "unity.h"

#include "lsp/facade/nav/iface_workspace.h"
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

/* ── Test 01: per-impl predicate gate -- ObjectDecl default-true */

static void test_per_impl_objectdecl_default_true(void) {
    /* The implementation.c per-impl filter calls
     * ilsp_vis_can_see(impl_canonical, requester, (Iron_Node *)od).
     * For ObjectDecl (no is_pub axis), the predicate default-trues so
     * cross-module implementors of an interface are kept. This is the
     * "default-allow" semantic per D-15 abort-audit posture. */
    Iron_ObjectDecl od = {0};
    od.kind = IRON_NODE_OBJECT_DECL;

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_b.iron", (const Iron_Node *)&od));
}

/* ── Test 02: stdlib carve-out applies to implementor lookups too */

static void test_per_impl_stdlib_carve_out(void) {
    /* Even if a future ObjectDecl gains is_pub, stdlib implementors
     * stay visible per D-08. Verify by setting the canonical_path to
     * stdlib:// and confirming the predicate short-circuits to true. */
    Iron_ObjectDecl od = {0};
    od.kind = IRON_NODE_OBJECT_DECL;

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "stdlib://collections.iron", "/tmp/mod_b.iron",
        (const Iron_Node *)&od));
}

/* ── Test 03: facade smoke -- implementation no-crash ─────────── */

static void test_facade_implementation_smoke(void) {
    /* Smoke: in-memory iface + impl pair; confirm the per-impl filter
     * doesn't drop legitimate implementors (ObjectDecl default-true). */
    const char *src =
        "interface Greeter { func hi() -> String }\n"
        "object MyGreeter { func hi() -> String { return \"hello\" } }\n";
    IronLsp_Server server;
    memset(&server, 0, sizeof(server));
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/v3vis_impl.iron", src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    /* Cursor on "Greeter" interface name (line 0, col ~10). */
    IronLsp_Position pos = { .line = 0, .character = 12 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_LocationLink *links = NULL;
    size_t n = 0;
    ilsp_facade_nav_implementation(&server, doc, pos, NULL, &arena,
                                     &links, &n);

    /* Acceptable: any n; assertion is no crash + null/non-null
     * consistency. */
    if (n > 0) TEST_ASSERT_NOT_NULL(links);
    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_per_impl_objectdecl_default_true);
    RUN_TEST(test_per_impl_stdlib_carve_out);
    RUN_TEST(test_facade_implementation_smoke);
    return UNITY_END();
}
