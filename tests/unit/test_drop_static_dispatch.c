/* Phase 24 DROP-03 (Plan 24-02): static-dispatch destructor — no vtable.
 *
 * Tests:
 *   1. Compiling an object with a drop block generates a <TypeName>_drop function.
 *   2. Generated C contains a direct `<TypeName>_drop(` call (static dispatch,
 *      NOT vtable lookup — DROP-03 guarantee).
 *   3. NO vtable or dispatch_table substring in the generated drop path.
 *
 * Pattern source: tests/unit/test_bvec_basic.c */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/emit_c.h"
#include "lir/lir_optimize.h"
#include "lir/lir.h"
#include "util/arena.h"

#include <string.h>

static Iron_Arena    g_arena;
static Iron_Arena    g_lir_arena;
static Iron_Arena    g_out_arena;
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena     = iron_arena_create(2 * 1024 * 1024);
    g_lir_arena = iron_arena_create(2 * 1024 * 1024);
    g_out_arena = iron_arena_create(2 * 1024 * 1024);
    g_diags     = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_out_arena);
    iron_arena_free(&g_lir_arena);
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* Full pipeline: lex -> parse -> analyze -> hir_lower -> hir_to_lir -> optimize -> emit_c.
 * Returns arena-allocated C source string, or NULL on pipeline failure. */
static const char *compile_to_c(const char *src, IronLIR_OptimizeInfo *opt_out) {
    Iron_AnalyzeResult res = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI_LENIENT,
        &g_arena, &g_diags, NULL, 0);

    if (res.has_errors || g_diags.error_count > 0) return NULL;

    IronHIR_Module *hir = iron_hir_lower(res.program, res.global_scope,
                                          NULL, &g_diags);
    if (!hir || g_diags.error_count > 0) return NULL;

    IronLIR_Module *lir = iron_hir_to_lir(hir, res.program, res.global_scope,
                                            &g_lir_arena, &g_diags);
    if (!lir || g_diags.error_count > 0) {
        iron_hir_module_destroy(hir);
        return NULL;
    }

    iron_lir_optimize(lir, opt_out, &g_out_arena, false, true, false);

    const char *c_src = iron_lir_emit_c(lir, &g_out_arena, &g_diags,
                                         opt_out, NULL, false, false);
    iron_hir_module_destroy(hir);
    return c_src;
}

/* Source: object T with a drop block; main heap-allocates, then frees.
 * Heap path triggers emit_ensure_drop which synthesises Iron_T_drop(Iron_T *self)
 * as a static function — verifying DROP-03 static dispatch with no vtable. */
static const char *kSrc =
    "object T {\n"
    "    val x: Int\n"
    "    drop {\n"
    "        var dummy = 0\n"
    "    }\n"
    "}\n"
    "func main() -> Int {\n"
    "    val t = heap T(1)\n"
    "    free t\n"
    "    return 0\n"
    "}\n";

/* ── Case 1: generated C contains a direct Iron_T_drop( call ──────────────── */

void test_drop_static_dispatch(void) {
    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(kSrc, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src, "compile_to_c returned NULL — check for diagnostics");

    /* DROP-03: drop before free — emit_ensure_drop synthesises the static wrapper.
     * emit_type_to_c prefixes 'Iron_' so the wrapper is named Iron_T_drop. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "Iron_T_drop("),
        "Expected Iron_T_drop( static destructor in generated C (DROP-03)");

    /* DROP-03: no vtable indirection allowed in the drop path.
     * Check for actual vtable dereference patterns (->vtable, vtable[), not
     * PHASE-26 placeholder comments which may mention "vtable" as future work. */
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "->vtable"),
        "Unexpected '->vtable' in generated C — drop must be static (DROP-03)");
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "vtable["),
        "Unexpected 'vtable[' in generated C — drop must be static (DROP-03)");
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "dispatch_table["),
        "Unexpected 'dispatch_table[' in generated C — drop must be static (DROP-03)");

    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_drop_static_dispatch);
    return UNITY_END();
}
