/* test_v3_visibility_type_definition -- Phase 10 Plan 10-02 (VIS-03).
 *
 * Asserts the cross-module visibility gate inside
 * ilsp_facade_nav_type_definition. Predicate-direct + facade smoke.
 *
 * Note per RESEARCH Conflict 3: ObjectDecl / InterfaceDecl / EnumDecl
 * have NO is_pub axis (parser drops the `private` keyword for them per
 * parser.c:4047). The predicate default-trues for ObjectDecl kind, so
 * the gate is functionally a no-op for object-typed values today --
 * the wiring is in place for Phase 11 PATCH / Phase 14 MIG when
 * additional decl kinds gain visibility bits.
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

/* ── Test 01: ObjectDecl default-true (RESEARCH Conflict 3) ───── */

static void test_objectdecl_default_true(void) {
    Iron_ObjectDecl od = {0};
    od.kind = IRON_NODE_OBJECT_DECL;

    /* ObjectDecl has no is_pub axis -> default-true. Cross-module
     * gate passes. */
    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_b.iron", (const Iron_Node *)&od));
}

/* ── Test 02: predicate same-module short-circuit (sanity) ───── */

static void test_objectdecl_same_module_short_circuit(void) {
    Iron_ObjectDecl od = {0};
    od.kind = IRON_NODE_OBJECT_DECL;

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_a.iron", (const Iron_Node *)&od));
}

/* ── Test 03: facade smoke -- typeDefinition no-crash ─────────── */

static void test_facade_type_definition_smoke(void) {
    /* Smoke: drive ilsp_facade_nav_type_definition; confirm the
     * VIS-03 gate insertion does not regress the existing same-file
     * resolution. */
    const char *src =
        "object Foo { var x: Int }\n"
        "func main() -> Int { val v: Foo = Foo{x: 1}; return v.x }\n";
    IronLsp_Server server;
    memset(&server, 0, sizeof(server));
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/v3vis_typedef.iron", src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    IronLsp_Position pos = { .line = 1, .character = 25 }; /* arbitrary */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_LocationLink *links = NULL;
    size_t n = 0;
    ilsp_facade_nav_type_definition(&server, doc, pos, NULL, &arena,
                                      &links, &n);

    if (n > 0) TEST_ASSERT_NOT_NULL(links);
    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_objectdecl_default_true);
    RUN_TEST(test_objectdecl_same_module_short_circuit);
    RUN_TEST(test_facade_type_definition_smoke);
    return UNITY_END();
}
