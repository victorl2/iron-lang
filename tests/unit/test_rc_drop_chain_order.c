/* Phase 26 POL-06 (Plan 26-03): rc drop chain order codegen test.
 *
 * Verifies the Plan 26-03 trampoline synthesis + iron_rc_alloc wiring:
 *
 *   1. Compiling an object with a user `drop {}` block and an
 *      `rc T(...)` allocation generates BOTH:
 *        - The Phase 24 <TypeName>_drop user-drop function
 *        - The Phase 26 <TypeName>_rc_drop void*-signature trampoline
 *      The trampoline delegates to <TypeName>_drop via an explicit
 *      `(<TypeName> *)self_void` cast.
 *
 *   2. The IRON_LIR_RC_ALLOC emission passes the trampoline pointer:
 *        iron_rc_alloc(sizeof(<Type>), <TypeName>_rc_drop)
 *      (NOT NULL — NULL is for types without drop need).
 *
 *   3. The drop chain order is preserved (Phase 24 inheritance):
 *      user drop body -> field destructors reverse-decl -> free.
 *      Verified by checking that the trampoline body calls into
 *      <TypeName>_drop (which compiles the user body + field destructors
 *      in reverse-decl order, per Phase 24 emit_ensure_drop:801).
 *
 *   4. A type WITHOUT a user drop block emits iron_rc_alloc(..., NULL).
 *      No trampoline lands in lifted_funcs.
 *
 * Pattern source: tests/unit/test_drop_static_dispatch.c. */

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

/* Full pipeline: lex -> parse -> analyze -> hir_lower -> hir_to_lir
 * -> optimize -> emit_c.
 * Returns arena-allocated C source string, or NULL on pipeline failure. */
static const char *compile_to_c(const char *src, IronLIR_OptimizeInfo *opt_out) {
    Iron_AnalyzeResult res = iron_analyze_buffer(
        src, strlen(src), "test_rc_drop_chain.iron",
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

    iron_lir_optimize(lir, opt_out, &g_out_arena, false, true);

    const char *c_src = iron_lir_emit_c(lir, &g_out_arena, &g_diags,
                                         opt_out, NULL, false, false);
    iron_hir_module_destroy(hir);
    return c_src;
}

/* Source 1: object with user drop block + rc allocation.
 * Triggers emit_ensure_rc_drop trampoline synthesis + iron_rc_alloc
 * with non-NULL drop_fn argument. */
static const char *kSrcWithDrop =
    "object Point {\n"
    "    val x: Int\n"
    "    val y: Int\n"
    "    drop {\n"
    "        var dummy = 0\n"
    "    }\n"
    "}\n"
    "func main() -> Int {\n"
    "    val p = rc Point(1, 2)\n"
    "    return p.x\n"
    "}\n";

/* Source 2: object WITHOUT user drop block. iron_rc_alloc should pass NULL;
 * no trampoline emitted. */
static const char *kSrcNoDrop =
    "object Plain {\n"
    "    val x: Int\n"
    "}\n"
    "func main() -> Int {\n"
    "    val p = rc Plain(42)\n"
    "    return p.x\n"
    "}\n";

/* ── Case 1: trampoline synthesis + drop chain ordering ─────────────── */
void test_rc_drop_chain_order_with_user_drop(void) {
    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(kSrcWithDrop, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src,
        "compile_to_c returned NULL on kSrcWithDrop -- check diagnostics");

    /* (a) Phase 24 <TypeName>_drop user destructor exists */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "Iron_Point_drop("),
        "Expected Iron_Point_drop( user destructor (Phase 24 emit_ensure_drop)");

    /* (b) Phase 26 <TypeName>_rc_drop trampoline exists */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(c_src, "Iron_Point_rc_drop("),
        "Expected Iron_Point_rc_drop( trampoline (Phase 26 emit_ensure_rc_drop)");

    /* (c) Trampoline signature is void*-erased: (void *self_void) */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "Iron_Point_rc_drop(void *self_void)"),
        "Trampoline must take a single void* arg (Iron_RcHeader.drop_fn signature)");

    /* (d) Trampoline body delegates to <TypeName>_drop via explicit cast */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "Iron_Point_drop((Iron_Point *)self_void)"),
        "Trampoline body must cast self_void to (Iron_Point *) and call Iron_Point_drop");

    /* (e) iron_rc_alloc emission passes the trampoline pointer (NOT NULL) */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "iron_rc_alloc(sizeof(Iron_Point), Iron_Point_rc_drop)"),
        "IRON_LIR_RC_ALLOC must pass <TypeName>_rc_drop as drop_fn");

    /* (f) Plan 26-03 banner present in trampoline comment */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "Phase 26 POL-06 (Plan 26-03)"),
        "Trampoline comment must reference Phase 26 POL-06 (Plan 26-03)");

    /* (g) NO vtable indirection (static dispatch only, GA5 lock) */
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "->vtable"),
        "Unexpected '->vtable' in rc drop path -- must be static dispatch");

    iron_lir_optimize_info_free(&opt);
}

/* ── Case 2: no-drop optimization — NULL drop_fn, no trampoline ─────── */
void test_rc_drop_chain_no_drop_optimization(void) {
    IronLIR_OptimizeInfo opt;
    const char *c_src = compile_to_c(kSrcNoDrop, &opt);
    TEST_ASSERT_NOT_NULL_MESSAGE(c_src,
        "compile_to_c returned NULL on kSrcNoDrop -- check diagnostics");

    /* (a) iron_rc_alloc emission passes NULL (no destructor needed) */
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(c_src, "iron_rc_alloc(sizeof(Iron_Plain), NULL)"),
        "Type without drop need must pass NULL drop_fn to iron_rc_alloc");

    /* (b) NO trampoline emitted in lifted_funcs (Anti-Pattern 4 lock) */
    TEST_ASSERT_NULL_MESSAGE(strstr(c_src, "Iron_Plain_rc_drop"),
        "No <TypeName>_rc_drop trampoline emitted for type without drop need");

    iron_lir_optimize_info_free(&opt);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rc_drop_chain_order_with_user_drop);
    RUN_TEST(test_rc_drop_chain_no_drop_optimization);
    return UNITY_END();
}
