/* HARD-07: concurrent-init smoke test for iron_types_init.
 *
 * The process-wide singleton s_primitives in src/analyzer/types.c is
 * initialised by iron_types_init. Before HARD-07 the init used a plain
 * `static bool s_initialized` flag, which was racy under concurrent
 * callers (two threads could both see !s_initialized and both run the
 * memset+stamp loop). Plan 04 converts the flag to pthread_once.
 *
 * This test is a concurrency smoke test:
 *   1. Two threads call iron_analyze_buffer on disjoint arenas — they both
 *      transitively call iron_types_init. Under TSAN (if enabled via
 *      -DIRON_ENABLE_SANITIZERS=ON) a data race here would fail the test.
 *   2. Four threads call iron_types_init directly 1000 times each. Must not
 *      crash or deadlock. Exercises the pthread_once wrapper directly.
 *
 * Without TSAN this still confirms no crash / no deadlock. Wall-clock
 * latency is not asserted — only observable progress.
 */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    Iron_Arena    arena;
    Iron_DiagList diags;
} worker_state_t;

static void *worker_run(void *arg) {
    worker_state_t *s = (worker_state_t *)arg;
    const char *src = "func w() -> Int { return 1 }\n";
    for (int i = 0; i < 100; i++) {
        iron_analyze_buffer(src, strlen(src), "concurrent.iron",
                            IRON_ANALYSIS_MODE_CLI,
                            &s->arena, &s->diags, NULL,
        0);
    }
    return NULL;
}

void test_two_threads_concurrent_iron_analyze_buffer_clean(void) {
    worker_state_t a = {
        .arena = iron_arena_create(131072),
        .diags = iron_diaglist_create()
    };
    worker_state_t b = {
        .arena = iron_arena_create(131072),
        .diags = iron_diaglist_create()
    };

    pthread_t ta, tb;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&ta, NULL, worker_run, &a));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tb, NULL, worker_run, &b));
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    /* Both workers should have analyzed clean files with zero errors. */
    TEST_ASSERT_EQUAL_INT(0, a.diags.error_count);
    TEST_ASSERT_EQUAL_INT(0, b.diags.error_count);

    iron_diaglist_free(&a.diags);
    iron_arena_free(&a.arena);
    iron_diaglist_free(&b.diags);
    iron_arena_free(&b.arena);
}

/* HARD-07: calling iron_types_init directly from 4 threads simultaneously
 * is safe (no crash, no deadlock). pthread_once guarantees the body runs
 * exactly once; subsequent calls are cheap. */
static void *direct_init_run(void *arg) {
    (void)arg;
    Iron_Arena a = iron_arena_create(1024);
    for (int i = 0; i < 1000; i++) {
        iron_types_init(&a);
    }
    iron_arena_free(&a);
    return NULL;
}

void test_concurrent_iron_types_init_is_safe(void) {
    pthread_t ts[4];
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&ts[i], NULL, direct_init_run, NULL));
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(ts[i], NULL);
    }
    TEST_ASSERT_TRUE(1); /* survival = pass */
}

/* HARD-07: after init, reading the primitive singletons from multiple threads
 * must yield identical (pointer-equal) results. This is the read-only
 * post-init invariant that lets HIR / LIR / typecheck compare primitive
 * types by pointer equality across threads. */
static void *read_int_singleton(void *arg) {
    Iron_Type **out = (Iron_Type **)arg;
    Iron_Arena a = iron_arena_create(512);
    iron_types_init(&a);
    *out = iron_type_make_primitive(IRON_TYPE_INT);
    iron_arena_free(&a);
    return NULL;
}

void test_primitive_singleton_pointer_equality_across_threads(void) {
    Iron_Type *p1 = NULL;
    Iron_Type *p2 = NULL;
    pthread_t  t1, t2;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t1, NULL, read_int_singleton, &p1));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t2, NULL, read_int_singleton, &p2));
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    TEST_ASSERT_EQUAL_INT(IRON_TYPE_INT, p1->kind);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_two_threads_concurrent_iron_analyze_buffer_clean);
    RUN_TEST(test_concurrent_iron_types_init_is_safe);
    RUN_TEST(test_primitive_singleton_pointer_equality_across_threads);
    return UNITY_END();
}
