/* test_stdlib_cache -- Phase 3 Plan 02 Task 02.
 *
 * Flips Wave 0 stub to real assertions. Covers:
 *   1. init populates known stdlib modules ("math", "io")
 *   2. idempotent init (pthread_once discipline)
 *   3. process-lifetime stability (1000 lookups yield same program ptr)
 *   4. multi-threaded lookup: N threads x M lookups, all same program ptr
 *   5. sealed/immutable surface: the Iron_Program.sealed flag may be
 *      false (parse-only cache skips analyzer), but the pointer MUST NOT
 *      change across concurrent reads (tested implicitly in 3+4).
 */
#include "unity.h"

#include "lsp/store/stdlib_cache.h"
#include "parser/ast.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* The CMake target-compile-definitions sets IRON_SOURCE_DIR=absolute-path
 * for ironls; tests get it via getenv("IRON_SOURCE_DIR") which we set
 * inline when needed.  Fall back to compile-time define if present. */
static const char *resolve_test_source_dir(void) {
    const char *env = getenv("IRON_SOURCE_DIR");
    if (env && *env) return env;
#ifdef IRON_SOURCE_DIR
    return IRON_SOURCE_DIR;
#else
    return "src";
#endif
}

/* ── 1. init populates known modules ─────────────────────────────────── */

static void test_init_populates_known_modules(void) {
    IronLsp_StdlibCache *c = ilsp_stdlib_cache_init(resolve_test_source_dir());
    TEST_ASSERT_NOT_NULL(c);

    /* math + io surface files ship in src/stdlib/; the parse-only cache
     * should load them. */
    const Iron_Program *math = ilsp_stdlib_cache_get(c, "math");
    TEST_ASSERT_NOT_NULL_MESSAGE(math, "math module should parse-cache");

    const Iron_Program *io = ilsp_stdlib_cache_get(c, "io");
    TEST_ASSERT_NOT_NULL_MESSAGE(io, "io module should parse-cache");

    /* Non-existent module returns NULL. */
    TEST_ASSERT_NULL(ilsp_stdlib_cache_get(c, "does_not_exist_xyz"));

    /* Size should be >= 2 (math + io at minimum). */
    TEST_ASSERT_GREATER_OR_EQUAL(2u, ilsp_stdlib_cache_size(c));
}

/* ── 2. idempotent init (pthread_once) ───────────────────────────────── */

static void test_idempotent_init(void) {
    IronLsp_StdlibCache *c1 = ilsp_stdlib_cache_init(resolve_test_source_dir());
    IronLsp_StdlibCache *c2 = ilsp_stdlib_cache_init(resolve_test_source_dir());
    TEST_ASSERT_EQUAL_PTR_MESSAGE(c1, c2,
        "pthread_once should return same singleton pointer");
}

/* ── 3. pointer stability across 1000 lookups ────────────────────────── */

static void test_pointer_stability(void) {
    IronLsp_StdlibCache *c = ilsp_stdlib_cache_init(resolve_test_source_dir());
    const Iron_Program *first = ilsp_stdlib_cache_get(c, "math");
    TEST_ASSERT_NOT_NULL(first);

    for (int i = 0; i < 1000; i++) {
        const Iron_Program *p = ilsp_stdlib_cache_get(c, "math");
        TEST_ASSERT_EQUAL_PTR(first, p);
    }
}

/* ── 4. multi-threaded concurrent lookup ─────────────────────────────── */

typedef struct {
    IronLsp_StdlibCache *cache;
    const Iron_Program  *expected;
    _Atomic int          mismatches;
} ThreadArg;

static void *lookup_thread(void *p) {
    ThreadArg *a = (ThreadArg *)p;
    for (int i = 0; i < 1000; i++) {
        const Iron_Program *prog = ilsp_stdlib_cache_get(a->cache, "math");
        if (prog != a->expected) atomic_fetch_add(&a->mismatches, 1);
    }
    return NULL;
}

static void test_concurrent_lookup(void) {
    IronLsp_StdlibCache *c = ilsp_stdlib_cache_init(resolve_test_source_dir());
    const Iron_Program *expected = ilsp_stdlib_cache_get(c, "math");
    TEST_ASSERT_NOT_NULL(expected);

    ThreadArg arg = { .cache = c, .expected = expected, .mismatches = 0 };

    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&threads[i], NULL, lookup_thread, &arg);
    }
    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, atomic_load(&arg.mismatches),
        "Concurrent lookups must return the same singleton pointer");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_populates_known_modules);
    RUN_TEST(test_idempotent_init);
    RUN_TEST(test_pointer_stability);
    RUN_TEST(test_concurrent_lookup);
    return UNITY_END();
}
