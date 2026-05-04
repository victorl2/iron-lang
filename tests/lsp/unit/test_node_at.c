/* test_node_at -- Phase 3 Plan 01 Task 04 (NAV-16).
 *
 * Covers:
 *   1. Cursor inside a decl identifier returns the covering decl node.
 *   2. Cursor in whitespace OUTSIDE every decl span returns NULL.
 *   3. Cursor past end-of-file returns NULL.
 *   4. Cursor on a nested method/field returns the innermost node,
 *      not the enclosing object decl.
 */
#include "unity.h"

#include "lsp/facade/nav/node_at.h"

#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Harness: build a doc + analyze. Caller frees via the paired helper. */
typedef struct {
    IronLsp_Document *doc;
    Iron_Arena        arena;
    Iron_DiagList     diags;
    Iron_Program     *program;
} NavHarness;

static void harness_init(NavHarness *h, const char *src, const char *uri) {
    h->arena = iron_arena_create(16 * 1024);
    h->diags = iron_diaglist_create();
    h->doc   = ilsp_document_create(uri, src, strlen(src), 1);
    Iron_AnalyzeResult r = iron_analyze_buffer(src, strlen(src), uri,
                                                IRON_ANALYSIS_MODE_CLI,
                                                &h->arena, &h->diags, NULL,
        0);
    h->program = r.program;
}

static void harness_free(NavHarness *h) {
    if (h->doc) ilsp_document_destroy(h->doc);
    iron_diaglist_free(&h->diags);
    iron_arena_free(&h->arena);
}

/* ── Test 01: cursor inside decl identifier returns the decl ─────────── */
static void test_cursor_on_func_name_returns_func_decl(void) {
    const char *src = "func foo() {}\n";
    NavHarness h;
    harness_init(&h, src, "/tmp/t.iron");
    TEST_ASSERT_NOT_NULL(h.program);

    /* Cursor on the 'o' of 'foo' (line 0, char 6). */
    IronLsp_Position pos = { .line = 0, .character = 6 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_FUNC_DECL, n->kind);

    harness_free(&h);
}

/* ── Test 02: cursor in whitespace between decls returns NULL ────────── */
static void test_cursor_in_whitespace_returns_null(void) {
    const char *src =
        "func a() {}\n"
        "\n"
        "func b() {}\n";
    NavHarness h;
    harness_init(&h, src, "/tmp/t.iron");
    TEST_ASSERT_NOT_NULL(h.program);

    /* Blank line 1 (0-based), col 0 -- outside any decl span. */
    IronLsp_Position pos = { .line = 1, .character = 0 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NULL(n);

    harness_free(&h);
}

/* ── Test 03: cursor outside file returns NULL ──────────────────────── */
static void test_cursor_past_eof_returns_null(void) {
    const char *src = "func a() {}\n";
    NavHarness h;
    harness_init(&h, src, "/tmp/t.iron");
    TEST_ASSERT_NOT_NULL(h.program);

    IronLsp_Position pos = { .line = 100, .character = 0 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NULL(n);

    harness_free(&h);
}

/* ── Test 04: cursor on nested field returns innermost node ──────────── */
static void test_cursor_on_nested_field_returns_field(void) {
    const char *src =
        "object Foo {\n"
        "    val x: Int\n"
        "}\n";
    NavHarness h;
    harness_init(&h, src, "/tmp/t.iron");
    TEST_ASSERT_NOT_NULL(h.program);

    /* Cursor on 'x' at line 1 (0-based), after "    val " = 8 chars. */
    IronLsp_Position pos = { .line = 1, .character = 8 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_NODE_FIELD, n->kind,
        "cursor on field should return the innermost Field node, not ObjectDecl");

    /* Cursor on 'Foo' at line 0 char 7 should return the object decl. */
    IronLsp_Position pos2 = { .line = 0, .character = 8 };
    Iron_Node *n2 = ilsp_nav_node_at(h.doc, h.program, pos2, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL(n2);
    TEST_ASSERT_EQUAL_INT(IRON_NODE_OBJECT_DECL, n2->kind);

    harness_free(&h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cursor_on_func_name_returns_func_decl);
    RUN_TEST(test_cursor_in_whitespace_returns_null);
    RUN_TEST(test_cursor_past_eof_returns_null);
    RUN_TEST(test_cursor_on_nested_field_returns_field);
    return UNITY_END();
}
