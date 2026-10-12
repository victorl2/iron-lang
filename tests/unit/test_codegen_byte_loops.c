/* Generated C for byte scanning loops and constant tables (#658): xs.len()
 * reads the count in place, bounds checks compare unsigned, and global
 * constant data is a static const with its initializer and no startup
 * assignment. The String accessors and fixed arrays, which need the CLI's
 * stdlib prepend and type rewrite, are covered by
 * tests/integration/codegen_byte_loops_smoke.sh. */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/emit_c.h"
#include "lir/lir_optimize.h"
#include "lir/lir.h"
#include "util/arena.h"
#include "runtime/iron_panic.h"  /* iron_panic_bvec_oob declaration */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

static const char *compile_to_c(const char *src, IronLIR_OptimizeInfo *opt_out) {
    Iron_AnalyzeResult res = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI_LENIENT,
        &g_arena, &g_diags, NULL, 0);

    if (res.has_errors || g_diags.error_count > 0) {
        if (getenv("DUMP_C")) iron_diag_print_all(&g_diags, src);
        return NULL;
    }

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

static void expect_has(const char *c, const char *needle) {
    char msg[256];
    snprintf(msg, sizeof msg, "expected in generated C: %s", needle);
    if (!strstr(c, needle) && getenv("DUMP_C")) fputs(c, stderr);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c, needle), msg);
}

static void expect_not(const char *c, const char *needle) {
    char msg[256];
    snprintf(msg, sizeof msg, "not expected in generated C: %s", needle);
    TEST_ASSERT_NULL_MESSAGE(strstr(c, needle), msg);
}

void test_list_len_is_inline(void) {
    static const char *src =
        "func sum(xs: [UInt8]) -> Int {\n"
        "    var t = 0\n"
        "    var i = 0\n"
        "    while i < xs.len() {\n"
        "        t += Int(xs[i])\n"
        "        i += 1\n"
        "    }\n"
        "    return t\n"
        "}\n"
        "func main() {\n"
        "    var xs: [UInt8] = [1, 2, 3]\n"
        "    xs.push(4)\n"
        "    println(\"{sum(xs)}\")\n"
        "}\n";
    IronLIR_OptimizeInfo opt;
    const char *c = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "compile_to_c returned NULL");
    expect_has(c, ")->count;");
    expect_not(c, "Iron_List_uint8_t_len(");
    /* the list index check is the unsigned compare clang folds */
    expect_has(c, "if ((uint64_t)");
    iron_lir_optimize_info_free(&opt);
}

void test_bvec_index_check_is_unsigned(void) {
    static const char *src =
        "func get(b: [Int; <=4], i: Int) -> Int {\n"
        "    return b[i]\n"
        "}\n"
        "func main() {\n"
        "    var b: [Int; <=4] = [1, 2]\n"
        "    b.push(3)\n"
        "    println(\"{get(b, 1)}\")\n"
        "}\n";
    IronLIR_OptimizeInfo opt;
    const char *c = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "compile_to_c returned NULL");
    expect_has(c, ") >= (uint64_t)");
    expect_not(c, " >= (int64_t)_v1.len)");
    iron_lir_optimize_info_free(&opt);
}

void test_global_constant_tables_are_static_const(void) {
    static const char *src =
        "val POW: [UInt64; <=4] = [1, 10, 100, 1000]\n"
        "val L = [1, 2, 3]\n"
        "val S = \"hello\"\n"
        "val N = 64\n"
        "var COUNTER = 0\n"
        "func main() {\n"
        "    COUNTER += 1\n"
        "    println(\"{POW[2]} {L[1]} {S} {N} {COUNTER}\")\n"
        "}\n";
    IronLIR_OptimizeInfo opt;
    const char *c = compile_to_c(src, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, "compile_to_c returned NULL");
    expect_has(c, "static const Iron_BVec_uint64_t_4 Iron_g_POW = { .len = 4, .data = {");
    expect_has(c, "static const int64_t Iron_g_L__items[3] = {");
    expect_has(c, "static const Iron_List_int64_t Iron_g_L = {");
    expect_has(c, "static const Iron_String Iron_g_S = { .sso = { .data = \"hello\", .len = 5 } };");
    expect_has(c, "static const int64_t Iron_g_N = ");
    /* a var keeps its mutable static */
    expect_has(c, "static int64_t Iron_g_COUNTER;");
    /* no startup assignment of the constant tables */
    expect_not(c, "Iron_List_int64_t_push(&");
    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_list_len_is_inline);
    RUN_TEST(test_bvec_index_check_is_unsigned);
    RUN_TEST(test_global_constant_tables_are_static_const);
    return UNITY_END();
}
