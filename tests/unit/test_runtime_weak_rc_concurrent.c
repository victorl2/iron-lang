/* test_runtime_weak_rc_concurrent.c — Phase 27 GA1 weak rc multi-thread stress.
 *
 * Wave 0 RED for Plan 27-01 Task 2; turns GREEN under Linux+TSan after the
 * weak-rc runtime substrate (iron_weak_rc_retain/release + extended
 * iron_rc_release block-free guard) is in place.
 *
 * Pattern reference: tests/unit/test_runtime_rc_concurrent.c (Phase 26
 * 8-thread × 100k retain/release stress). Adapted for weak-rc semantics: a
 * SINGLE shared rc allocation is exercised by 8 threads doing interleaved
 * strong-retain/strong-release/weak-retain/weak-release sequences. After
 * all threads join, both counters return to baseline (refcount=1 main-held;
 * weak_count=0).
 *
 * Atomic discipline being exercised (CONTEXT.md GA1):
 *   - strong retain:  IRON_ATOMIC_U64_FETCH_ADD_RELAXED on refcount
 *   - strong release: IRON_ATOMIC_U64_FETCH_SUB_RELEASE on refcount
 *   - on prev == 1:   IRON_ATOMIC_FENCE_ACQUIRE + drop_fn + conditional free
 *                     gated by ACQUIRE-load on weak_count
 *   - weak retain:    IRON_ATOMIC_U64_FETCH_ADD_RELAXED on weak_count
 *   - weak release:   IRON_ATOMIC_U64_FETCH_SUB_RELAXED on weak_count;
 *                     conditional free gated by ACQUIRE-load on refcount
 *
 * The CMake test entry sets FAIL_REGULAR_EXPRESSION "WARNING: ThreadSanitizer"
 * so any TSan data-race or ordering complaint converts to a test failure.
 *
 * Platform gate: same as Phase 26 test_runtime_rc_concurrent — Linux+TSan
 * only. Non-TSan builds SKIP cleanly so the phase27-invariant label stays
 * green on macOS / non-TSan Linux.
 *
 * Known issue inheritance (Phase 26 Plan 03 SUMMARY.md #1): podman's
 * Rocky Linux 9 image may need a libtsan.so symlink for TSan-instrumented
 * binaries to run:
 *
 *     ln -sf /usr/lib/gcc/x86_64-redhat-linux/11/libtsan.so \
 *            /usr/lib64/libtsan.so.0
 *
 * If the binary BAD_COMMANDs on podman with "tsan runtime missing", apply
 * the workaround and re-run. This test is allowed to SKIP under that
 * known-issue path; non-blocking for the phase27-invariant gate.
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

#define WEAK_RC_STRESS_THREADS  8
#define WEAK_RC_STRESS_ITERS    100000

/* drop_fn invocation counter — must be 1 at end of test. */
static int g_weak_concurrent_drop_calls = 0;

static void weak_concurrent_drop(void *self) {
    (void)self;
    g_weak_concurrent_drop_calls++;
}

/* Per-thread worker: interleave 4 ops per iteration —
 *   strong retain → weak retain → weak release → strong release.
 * Net effect per iteration: zero (both counters unchanged at iteration
 * boundary). After all threads join: refcount == 1 (main-held baseline),
 * weak_count == 0. main's iron_rc_release fires drop_fn exactly once. */
static void *weak_rc_stress_worker(void *arg) {
    void *shared = arg;
    for (int i = 0; i < WEAK_RC_STRESS_ITERS; i++) {
        iron_rc_retain(shared);
        iron_weak_rc_retain(shared);
        iron_weak_rc_release(shared);
        iron_rc_release(shared);
    }
    return NULL;
}

void test_iron_weak_rc_concurrent_interleaved_balanced(void) {
    g_weak_concurrent_drop_calls = 0;

    /* Allocate ONE shared rc, refcount starts at 1, weak_count at 0. */
    void *shared = iron_rc_alloc(sizeof(int64_t), weak_concurrent_drop);
    TEST_ASSERT_NOT_NULL(shared);
    *(int64_t *)shared = 0x1234567890ABCDEFull;

    Iron_RcHeader *rch = iron_rc_header_of(shared);
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_UINT64(0, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    /* 8 threads × 100k interleaved retain/release sequences. */
    pthread_t threads[WEAK_RC_STRESS_THREADS];
    for (int i = 0; i < WEAK_RC_STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0,
            pthread_create(&threads[i], NULL, weak_rc_stress_worker, shared));
    }
    for (int i = 0; i < WEAK_RC_STRESS_THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
    }

    /* After all threads join, both counters must be at their baselines. */
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_UINT64(0, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));
    TEST_ASSERT_EQUAL_INT(0, g_weak_concurrent_drop_calls);

    /* Final release on main — drop_fn fires once (weak_count==0 so block
     * is freed in the same call). */
    iron_rc_release(shared);
    TEST_ASSERT_EQUAL_INT(1, g_weak_concurrent_drop_calls);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_weak_rc_concurrent_interleaved_balanced);
    return UNITY_END();
}

#else  /* not Linux+TSan */

void test_weak_rc_concurrent_tsan_only(void) {
    TEST_IGNORE_MESSAGE("weak rc concurrent stress is Linux+TSan only — "
                        "compile with -fsanitize=thread on Linux to enable. "
                        "Default builds skip this case cleanly so the "
                        "phase27-invariant label stays green on macOS / "
                        "non-TSan Linux. "
                        "Known issue: podman Rocky 9 may need libtsan symlink "
                        "(Phase 26 Plan 03 SUMMARY Issue #1).");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_weak_rc_concurrent_tsan_only);
    return UNITY_END();
}

#endif  /* Linux+TSan gate */
