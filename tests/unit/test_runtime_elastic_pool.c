/* test_runtime_elastic_pool.c — Unity tests for the Phase 59 P01b elastic
 * Iron_Pool extension, Iron_io_pool global, Iron_PoolWait abandoned-flag
 * coordination primitive, and Iron_pool_mark_one_leaked bookkeeping.
 *
 * Covers:
 *   1. test_iron_io_pool_wired       — iron_threads_init() wires Iron_io_pool
 *      as an elastic pool with max_threads=64 and thread_count==0.
 *   2. test_elastic_grow_shrink      — submit 3 quick no-ops to a small
 *      elastic pool, assert thread_count grew > 0 during submit and shrank
 *      back toward 0 within ~300ms of idle.
 *   3. test_poolwait_timeout         — Iron_poolwait_wait_ms(100) with no
 *      signaler returns 0 after ~100ms.
 *   4. test_poolwait_completes       — worker finishes, wait returns 1,
 *      Iron_poolwait_completed() is true.
 *   5. test_pool_mark_one_leaked     — Iron_pool_mark_one_leaked increments
 *      pool->leaked_count.
 *
 * Uses the existing iron_threads_init / iron_threads_shutdown lifecycle
 * (not iron_runtime_init — that wiring lives in P01c).
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* iron_threads_init / iron_threads_shutdown are internal lifecycle hooks
 * not yet exposed in iron_runtime.h — declare locally. */
extern void iron_threads_init(void);
extern void iron_threads_shutdown(void);

/* ── Internal accessors ──────────────────────────────────────────────────────
 * P01b ships a small read accessor so tests can observe elastic pool state
 * without depending on the internal struct layout. These live in
 * iron_threads.c alongside the elastic impl.
 */
extern bool Iron_pool_is_elastic(const Iron_Pool *p);
extern int  Iron_pool_max_threads(const Iron_Pool *p);
extern int  Iron_pool_live_thread_count(const Iron_Pool *p);
extern int  Iron_pool_leaked_count(const Iron_Pool *p);

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_threads_init(); }
void tearDown(void) { iron_threads_shutdown(); }

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void noop_work_fn(void *arg) {
    (void)arg;
    /* intentionally trivial — just let the worker finish fast */
}

static void short_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

void test_iron_io_pool_wired(void) {
    TEST_ASSERT_NOT_NULL_MESSAGE(Iron_io_pool,
        "iron_threads_init should have created Iron_io_pool");
    TEST_ASSERT_TRUE_MESSAGE(Iron_pool_is_elastic(Iron_io_pool),
        "Iron_io_pool should be elastic");
    TEST_ASSERT_EQUAL_INT_MESSAGE(64, Iron_pool_max_threads(Iron_io_pool),
        "Iron_io_pool max_threads should be 64");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, Iron_pool_live_thread_count(Iron_io_pool),
        "Iron_io_pool should start with 0 live worker threads");
}

void test_elastic_grow_shrink(void) {
    Iron_Pool *p = Iron_elastic_pool_create("test-grow", 4, 100);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(Iron_pool_is_elastic(p));
    TEST_ASSERT_EQUAL_INT(4, Iron_pool_max_threads(p));
    TEST_ASSERT_EQUAL_INT(0, Iron_pool_live_thread_count(p));

    /* Submit three no-ops — elastic math should spawn at least one worker. */
    Iron_pool_submit(p, noop_work_fn, NULL);
    Iron_pool_submit(p, noop_work_fn, NULL);
    Iron_pool_submit(p, noop_work_fn, NULL);

    /* Barrier so we know all submitted work completed before we poll
     * for shrinkage. The barrier returns once pending == 0. */
    Iron_pool_barrier(p);

    int peak = Iron_pool_live_thread_count(p);
    /* At least one worker must have been spawned to run the work. */
    TEST_ASSERT_TRUE_MESSAGE(peak > 0,
        "elastic pool should grow thread_count > 0 on submit");
    TEST_ASSERT_TRUE_MESSAGE(peak <= 4,
        "elastic pool should not exceed max_threads");

    /* Wait past idle_timeout_ms (100) + margin. Workers retire themselves
     * from their slots on IRON_TIMEDWAIT_EXPIRED with an empty queue. */
    short_sleep_ms(350);

    int after = Iron_pool_live_thread_count(p);
    TEST_ASSERT_TRUE_MESSAGE(after <= peak,
        "elastic pool thread_count should not grow while idle");
    TEST_ASSERT_TRUE_MESSAGE(after == 0,
        "elastic pool should shrink back to 0 after idle_timeout");

    Iron_pool_destroy(p);
}

void test_poolwait_timeout(void) {
    Iron_PoolWait *w = Iron_poolwait_create();
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(Iron_poolwait_completed(w));

    uint64_t t0 = Iron_monotonic_now_ms();
    int rc = Iron_poolwait_wait_ms(w, 100);
    uint64_t t1 = Iron_monotonic_now_ms();

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc,
        "Iron_poolwait_wait_ms should return 0 (timeout) with no signaler");

    uint64_t elapsed = t1 - t0;
    TEST_ASSERT_TRUE_MESSAGE(elapsed >= 50,
        "Iron_poolwait_wait_ms returned too early (< 50ms)");
    TEST_ASSERT_TRUE_MESSAGE(elapsed <= 400,
        "Iron_poolwait_wait_ms returned too late (> 400ms)");

    TEST_ASSERT_FALSE(Iron_poolwait_completed(w));
    Iron_poolwait_destroy(w);
}

/* Background thread used by test_poolwait_completes: sleeps then signals. */
static void *signaler_thread_fn(void *arg) {
    Iron_PoolWait *w = (Iron_PoolWait *)arg;
    short_sleep_ms(50);
    Iron_poolwait_worker_finish(w, NULL, NULL);
    return NULL;
}

void test_poolwait_completes(void) {
    Iron_PoolWait *w = Iron_poolwait_create();
    TEST_ASSERT_NOT_NULL(w);

    pthread_t signaler;
    int err = pthread_create(&signaler, NULL, signaler_thread_fn, w);
    TEST_ASSERT_EQUAL_INT(0, err);

    uint64_t t0 = Iron_monotonic_now_ms();
    int rc = Iron_poolwait_wait_ms(w, 2000);
    uint64_t t1 = Iron_monotonic_now_ms();

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, rc,
        "Iron_poolwait_wait_ms should return 1 (completed) once signaled");
    TEST_ASSERT_TRUE_MESSAGE(Iron_poolwait_completed(w),
        "Iron_poolwait_completed should be true after worker_finish");

    /* Should not have burned the full 2s budget. */
    uint64_t elapsed = t1 - t0;
    TEST_ASSERT_TRUE_MESSAGE(elapsed < 1500,
        "wait should return shortly after signal, not hit full timeout");

    pthread_join(signaler, NULL);
    Iron_poolwait_destroy(w);
}

void test_pool_mark_one_leaked(void) {
    Iron_Pool *p = Iron_elastic_pool_create("test-leaked", 4, 100);
    TEST_ASSERT_NOT_NULL(p);

    int before = Iron_pool_leaked_count(p);

    /* Submit a no-op so pending > 0 and a worker spawns (otherwise the
     * pending-- inside mark_one_leaked would be clamped). */
    Iron_pool_submit(p, noop_work_fn, NULL);
    Iron_pool_barrier(p);

    /* Now artificially flag one worker as leaked. */
    Iron_pool_mark_one_leaked(p);

    int after = Iron_pool_leaked_count(p);
    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, after,
        "Iron_pool_mark_one_leaked should increment leaked_count by 1");

    /* Pool must still be destroyable without hanging or crashing. */
    Iron_pool_destroy(p);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_io_pool_wired);
    RUN_TEST(test_elastic_grow_shrink);
    RUN_TEST(test_poolwait_timeout);
    RUN_TEST(test_poolwait_completes);
    RUN_TEST(test_pool_mark_one_leaked);
    return UNITY_END();
}
