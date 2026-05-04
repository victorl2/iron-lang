/* test_document_symbol -- Phase 3 Plan 03 Task 03 (NAV-07, D-10).
 *
 * Flipped from the Plan 01 Wave 0 stub. Drives
 * ilsp_facade_nav_document_symbol against minimal in-memory fixtures
 * and asserts the hierarchical DocumentSymbol tree shape per D-10.
 *
 * Four RUN_TESTs:
 *   1. top-level funcs emit source order: a, b, c.
 *   2. object Foo with fields produces Class-kind parent + Field kids.
 *   3. enum Color with variants produces Enum + EnumMember kids.
 *   4. empty file returns empty array.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define LSP_SYMKIND_CLASS       5
#define LSP_SYMKIND_FIELD       8
#define LSP_SYMKIND_ENUM        10
#define LSP_SYMKIND_INTERFACE   11
#define LSP_SYMKIND_FUNCTION    12
#define LSP_SYMKIND_ENUMMEMBER  22

typedef struct {
    IronLsp_Server    server;
    IronLsp_Document *doc;
} fx_t;

static void fx_init(fx_t *f, const char *uri, const char *src) {
    memset(f, 0, sizeof(*f));
    f->server.position_encoding = ILSP_ENC_UTF16;
    f->doc = ilsp_document_create(uri, src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(f->doc);
}

static void fx_destroy(fx_t *f) {
    if (f->doc) ilsp_document_destroy(f->doc);
    memset(f, 0, sizeof(*f));
}

/* ── Test 01: source-order emission of top-level funcs ───────────── */

static void test_source_order_functions(void) {
    const char *src =
        "func a() {}\n"
        "func b() {}\n"
        "func c() {}\n";
    fx_t f;
    fx_init(&f, "/tmp/t_src_order.iron", src);

    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_DocSymbol *syms = NULL;
    size_t n = 0;
    ilsp_facade_nav_document_symbol(&f.server, f.doc, NULL, &arena,
                                      &syms, &n, true);

    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_NOT_NULL(syms);
    TEST_ASSERT_EQUAL_STRING("a", syms[0].name);
    TEST_ASSERT_EQUAL_STRING("b", syms[1].name);
    TEST_ASSERT_EQUAL_STRING("c", syms[2].name);
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_FUNCTION, syms[0].kind);
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_FUNCTION, syms[1].kind);
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_FUNCTION, syms[2].kind);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 02: object -> class kind + children ────────────────────── */

static void test_object_with_fields(void) {
    const char *src =
        "object Foo {\n"
        "    val x: Int\n"
        "    val y: Int\n"
        "}\n";
    fx_t f;
    fx_init(&f, "/tmp/t_object.iron", src);

    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_DocSymbol *syms = NULL;
    size_t n = 0;
    ilsp_facade_nav_document_symbol(&f.server, f.doc, NULL, &arena,
                                      &syms, &n, true);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_STRING("Foo", syms[0].name);
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_CLASS, syms[0].kind);

    /* Children = 2 fields. */
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1, syms[0].child_count);
    /* The first field must be kind Field. */
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_FIELD, syms[0].children[0].kind);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 03: enum with variants ─────────────────────────────────── */

static void test_enum_with_variants(void) {
    const char *src =
        "enum Color {\n"
        "    Red,\n"
        "    Green,\n"
        "    Blue,\n"
        "}\n";
    fx_t f;
    fx_init(&f, "/tmp/t_enum.iron", src);

    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_DocSymbol *syms = NULL;
    size_t n = 0;
    ilsp_facade_nav_document_symbol(&f.server, f.doc, NULL, &arena,
                                      &syms, &n, true);

    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_STRING("Color", syms[0].name);
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_ENUM, syms[0].kind);
    TEST_ASSERT_EQUAL_size_t(3, syms[0].child_count);
    TEST_ASSERT_EQUAL_STRING("Red",   syms[0].children[0].name);
    TEST_ASSERT_EQUAL_STRING("Green", syms[0].children[1].name);
    TEST_ASSERT_EQUAL_STRING("Blue",  syms[0].children[2].name);
    TEST_ASSERT_EQUAL_INT(LSP_SYMKIND_ENUMMEMBER, syms[0].children[0].kind);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 04: empty file ─────────────────────────────────────────── */

static void test_empty_file_returns_empty(void) {
    const char *src = "\n";
    fx_t f;
    fx_init(&f, "/tmp/t_empty.iron", src);

    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_DocSymbol *syms = NULL;
    size_t n = 0;
    ilsp_facade_nav_document_symbol(&f.server, f.doc, NULL, &arena,
                                      &syms, &n, true);

    TEST_ASSERT_EQUAL_size_t(0, n);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_source_order_functions);
    RUN_TEST(test_object_with_fields);
    RUN_TEST(test_enum_with_variants);
    RUN_TEST(test_empty_file_returns_empty);
    return UNITY_END();
}
