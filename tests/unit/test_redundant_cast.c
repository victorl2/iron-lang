/* test_redundant_cast.c — Phase 4 Plan 04-01 Task 02 (EDIT-07).
 *
 * LIVE — exercises the redundant-cast warning branch in the primitive-
 * cast handler in src/analyzer/typecheck.c. Asserts
 * IRON_WARN_REDUNDANT_CAST (612) fires when the source type already
 * matches the cast target, and stays quiet on narrowing casts that
 * still carry the IRON_WARN_NARROWING_CAST signal.
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

static const Iron_Diagnostic *find_diag(const Iron_DiagList *dl, int code) {
    for (int i = 0; i < dl->count; i++) {
        if (dl->items[i].code == code) return &dl->items[i];
    }
    return NULL;
}

static void analyze(const char *src) {
    (void)iron_analyze_buffer(src, strlen(src), "test.iron",
                               IRON_ANALYSIS_MODE_CLI,
                               &g_arena, &g_diags, NULL,
        0);
}

/* Float(1.0) is a redundant cast of a Float literal to Float. */
static void test_same_type_cast_fires_612(void) {
    const char *src =
        "func main() {\n"
        "  val x = Float(1.0)\n"
        "}\n";
    analyze(src);
    const Iron_Diagnostic *d = find_diag(&g_diags, IRON_WARN_REDUNDANT_CAST);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_NULL(d->suggestion);
    TEST_ASSERT_EQUAL_STRING("1.0", d->suggestion);
}

/* Int32(some-Int-var) is a narrowing cast — should NOT fire 612. */
static void test_narrowing_cast_stays_quiet(void) {
    const char *src =
        "func main() {\n"
        "  val y = 1\n"
        "  val x = Int32(y)\n"
        "}\n";
    analyze(src);
    TEST_ASSERT_NULL(find_diag(&g_diags, IRON_WARN_REDUNDANT_CAST));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_same_type_cast_fires_612);
    RUN_TEST(test_narrowing_cast_stays_quiet);
    return UNITY_END();
}
