/* test_rc_op_counter.c — Phase 29 Plan 04 (OPT-08 input): opt-in rc-op counter.
 *
 * This test is built WITH -DIRON_RC_COUNT (set via target_compile_definitions
 * in tests/unit/CMakeLists.txt), turning ON the `_Atomic uint64_t` retain/
 * release counters that iron_rc.c compiles out by default. It asserts the
 * counters increment exactly once per iron_rc_retain / iron_rc_release call
 * so the deferred OPT-08 `arc`-policy benchmark (OQ-07) has a verified data
 * source. The counter is OFF in normal builds (no atomic in the hot path);
 * this test exists solely to prove the instrumented path works.
 *
 * Oracle: N retains + M releases → iron_rc_op_counts() returns (N, M).
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <stdint.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* A drop_fn that does nothing — we never let refcount reach 0 in the count
 * tests so the block stays live; the final release in each test takes it to
 * exactly 1 (the alloc seed) before we free explicitly via a final release. */
static void noop_drop(void *p) { (void)p; }

/* The counter must exist and be readable even though this TU is compiled WITH
 * IRON_RC_COUNT — the accessor is always declared in the header. */
void test_op_counter_starts_at_zero(void) {
    iron_rc_op_counts_reset();
    uint64_t retains = 1, releases = 1;
    iron_rc_op_counts(&retains, &releases);
    TEST_ASSERT_EQUAL_UINT64(0, retains);
    TEST_ASSERT_EQUAL_UINT64(0, releases);
}

void test_op_counter_counts_retains(void) {
    iron_rc_op_counts_reset();
    void *p = iron_rc_alloc(16, noop_drop);   /* refcount = 1, NOT a retain */
    iron_rc_retain(p);                         /* refcount = 2, retain #1 */
    iron_rc_retain(p);                         /* refcount = 3, retain #2 */
    iron_rc_retain(p);                         /* refcount = 4, retain #3 */

    uint64_t retains = 0, releases = 0;
    iron_rc_op_counts(&retains, &releases);
    TEST_ASSERT_EQUAL_UINT64(3, retains);
    TEST_ASSERT_EQUAL_UINT64(0, releases);

    /* Drain the 4 references (3 retains + 1 alloc seed) so the block frees. */
    iron_rc_release(p);
    iron_rc_release(p);
    iron_rc_release(p);
    iron_rc_release(p);
}

void test_op_counter_counts_releases(void) {
    iron_rc_op_counts_reset();
    void *p = iron_rc_alloc(16, noop_drop);   /* refcount = 1 */
    iron_rc_retain(p);                         /* refcount = 2 */
    iron_rc_retain(p);                         /* refcount = 3 */

    iron_rc_release(p);                        /* refcount = 2, release #1 */
    iron_rc_release(p);                        /* refcount = 1, release #2 */
    iron_rc_release(p);                        /* refcount = 0, release #3 -> free */

    uint64_t retains = 0, releases = 0;
    iron_rc_op_counts(&retains, &releases);
    TEST_ASSERT_EQUAL_UINT64(2, retains);
    TEST_ASSERT_EQUAL_UINT64(3, releases);
}

void test_op_counter_null_args_safe(void) {
    iron_rc_op_counts_reset();
    void *p = iron_rc_alloc(8, noop_drop);
    iron_rc_retain(p);
    /* NULL out-params must not crash — accessor tolerates partial reads. */
    iron_rc_op_counts(NULL, NULL);
    uint64_t retains = 0;
    iron_rc_op_counts(&retains, NULL);
    TEST_ASSERT_EQUAL_UINT64(1, retains);
    iron_rc_release(p);
    iron_rc_release(p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_op_counter_starts_at_zero);
    RUN_TEST(test_op_counter_counts_retains);
    RUN_TEST(test_op_counter_counts_releases);
    RUN_TEST(test_op_counter_null_args_safe);
    return UNITY_END();
}
