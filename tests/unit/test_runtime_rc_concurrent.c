/* test_runtime_rc_concurrent.c — Phase 26 POL-06 multi-thread refcount stress.
 *
 * Wave 0 RED for Plan 26-01 Task 1; turns GREEN under Linux+TSan after Plan
 * 26-01 Task 2 lands iron_rc_alloc / iron_rc_retain / iron_rc_release.
 *
 * Pattern reference: tests/unit/test_runtime_heap_concurrent.c (Phase 19
 * 8-thread × 100k iteration stress). Adapted for refcount semantics: a
 * SINGLE shared rc allocation is retained + released concurrently, and the
 * invariant is `final_refcount == 1` (one extra retain held by main thread
 * for the test scaffold) after pthread_join.
 *
 * Atomic discipline being exercised (CONTEXT.md GA3):
 *   - retain:  IRON_ATOMIC_U64_FETCH_ADD_RELAXED   (relaxed-inc)
 *   - release: IRON_ATOMIC_U64_FETCH_SUB_RELEASE   (release-dec)
 *   - on prev == 1: IRON_ATOMIC_FENCE_ACQUIRE before drop_fn
 *
 * The CMake test entry sets FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer"
 * so any TSan data-race or ordering complaint converts to a test failure.
 *
 * Platform gate: this test only meaningfully exercises atomic ordering when
 * compiled under Linux+TSan. On macOS or non-TSan Linux builds, the test
 * SKIPs with TEST_IGNORE_MESSAGE so the CMake invariant set still passes.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdint.h>
#include <stdio.h>

void setUp(void)    {}
void tearDown(void) {}

#if defined(__linux__) && defined(__SANITIZE_THREAD__)

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RC_STRESS_THREADS  8
#define RC_STRESS_ITERS    100000

/* drop_fn invocation counter — must be 1 at end of test. */
static int g_concurrent_drop_calls = 0;

static void concurrent_drop(void *self) {
    (void)self;
    g_concurrent_drop_calls++;
}

/* Per-thread worker: NxRETAIN paired with NxRELEASE on the shared pointer.
 * Net effect: zero. After all 8 threads join, refcount == 1 (the main-thread
 * baseline retain). main releases once more → drop_fn fires exactly once. */
static void *rc_stress_worker(void *arg) {
    void *shared = arg;
    for (int i = 0; i < RC_STRESS_ITERS; i++) {
        iron_rc_retain(shared);
        iron_rc_release(shared);
    }
    return NULL;
}

void test_iron_rc_concurrent_retain_release_balanced(void) {
    g_concurrent_drop_calls = 0;

    /* Allocate ONE shared rc, refcount starts at 1. */
    void *shared = iron_rc_alloc(sizeof(int64_t), concurrent_drop);
    TEST_ASSERT_NOT_NULL(shared);
    *(int64_t *)shared = 0x1234567890ABCDEFull;

    Iron_RcHeader *rch = iron_rc_header_of(shared);
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));

    /* 8 threads × 100k balanced retain/release pairs. */
    pthread_t threads[RC_STRESS_THREADS];
    for (int i = 0; i < RC_STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0,
            pthread_create(&threads[i], NULL, rc_stress_worker, shared));
    }
    for (int i = 0; i < RC_STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
    }

    /* After all threads join, refcount must be exactly 1 again. Any TSan
     * warning during the run converts via CMake FAIL_REGULAR_EXPRESSION. */
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_INT(0, g_concurrent_drop_calls);

    /* Final release on main thread — drop_fn fires once, observes the
     * sentinel payload (acquire-fence synchronizes with all worker
     * releases). */
    iron_rc_release(shared);
    TEST_ASSERT_EQUAL_INT(1, g_concurrent_drop_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_rc_concurrent_retain_release_balanced);
    return UNITY_END();
}

#else  /* not Linux+TSan */

void test_rc_concurrent_tsan_only(void) {
    TEST_IGNORE_MESSAGE("rc concurrent stress is Linux+TSan only — "
                        "compile with -fsanitize=thread on Linux to enable. "
                        "Default builds skip this case cleanly so the "
                        "phase26-invariant label stays green on macOS / "
                        "non-TSan Linux.");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rc_concurrent_tsan_only);
    return UNITY_END();
}

#endif  /* Linux+TSan gate */
