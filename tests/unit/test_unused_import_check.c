/* test_unused_import_check.c — Phase 4 Plan 04-01 Task 02 (EDIT-07).
 *
 * Import aliases are rejected with IRON_ERR_IMPORT_NOT_FOUND; the unused
 * alias warning (611) that aliased imports used to get stays quiet.
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stddef.h>
#include <string.h>

static Iron_Arena    g_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 256);
    g_diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

static int count_code(const Iron_DiagList *dl, int code) {
    int n = 0;
    for (int i = 0; i < dl->count; i++) if (dl->items[i].code == code) n++;
    return n;
}

static void analyze(const char *src) {
    (void)iron_analyze_buffer(src, strlen(src), "test.iron",
                               IRON_ANALYSIS_MODE_CLI,
                               &g_arena, &g_diags, NULL,
        0);
}

/* Import aliases are rejected (E0209), and the unused-alias warning
 * they used to get no longer fires on top of the error. */
static void test_used_import_stays_quiet(void) {
    const char *src =
        "import std.math as m\n"
        "func main() {\n"
        "  val x = m\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_EQUAL_INT(0, count_code(&g_diags, IRON_WARN_UNUSED_IMPORT));
    TEST_ASSERT_EQUAL_INT(1, count_code(&g_diags, IRON_ERR_IMPORT_NOT_FOUND));
}

static void test_unused_import_fires_611(void) {
    const char *src =
        "import std.math as m\n"
        "func main() {}\n";
    analyze(src);
    TEST_ASSERT_EQUAL_INT(0, count_code(&g_diags, IRON_WARN_UNUSED_IMPORT));
    TEST_ASSERT_EQUAL_INT(1, count_code(&g_diags, IRON_ERR_IMPORT_NOT_FOUND));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_used_import_stays_quiet);
    RUN_TEST(test_unused_import_fires_611);
    return UNITY_END();
}
