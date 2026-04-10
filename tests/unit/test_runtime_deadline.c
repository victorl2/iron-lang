/* test_runtime_deadline.c — Unity tests for Iron_Deadline / monotonic clock /
 * iron_cond_timedwait_ms (Phase 59 P01a foundation).
 *
 * Covers:
 *   1. Iron_monotonic_now_ms is strictly non-decreasing across 1000 calls.
 *   2. Iron_deadline_from_timeout_ms(0)  -> expired() == true,  remaining_ms() == 0.
 *   3. Iron_deadline_from_timeout_ms(-1) -> expired() == true (negative == poll-once).
 *   4. Iron_deadline_from_timeout_ms(5000) -> remaining in (0,5000], not expired.
 *   5. Iron_deadline_from_timeout_ms(INT64_MAX) -> remaining clamped to 0x7FFFFFFF.
 *   6. Deadline with deadline_mono_ms = 1 (long in the past) -> expired, remaining 0.
 *   7. iron_cond_timedwait_ms(100) with no signaler returns IRON_TIMEDWAIT_EXPIRED
 *      after ~100ms (tolerance 50..400ms).
 *
 * NOTE: uses raw iron_mutex_t / iron_cond_t from runtime/iron_runtime.h and the
 * IRON_MUTEX_INIT / IRON_COND_INIT / IRON_MUTEX_LOCK / IRON_MUTEX_UNLOCK /
 * IRON_MUTEX_DESTROY / IRON_COND_DESTROY cross-platform macros already present
 * in iron_runtime.h — no new threading abstraction is introduced.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { /* no runtime init needed — deadline/clock are pure */ }
void tearDown(void) { }

/* ── Tests ───────────────────────────────────────────────────────────────── */

void test_monotonic_non_decreasing(void) {
    uint64_t prev = Iron_monotonic_now_ms();
    for (int i = 0; i < 1000; i++) {
        uint64_t now = Iron_monotonic_now_ms();
        TEST_ASSERT_TRUE_MESSAGE(now >= prev, "Iron_monotonic_now_ms went backwards");
        prev = now;
    }
}

void test_deadline_from_zero_timeout(void) {
    Iron_Deadline d = Iron_deadline_from_timeout_ms(0);
    TEST_ASSERT_TRUE(Iron_deadline_expired(d));
    TEST_ASSERT_EQUAL_INT(0, Iron_deadline_remaining_ms(d));
}

void test_deadline_from_negative_timeout(void) {
    Iron_Deadline d = Iron_deadline_from_timeout_ms(-1);
    TEST_ASSERT_TRUE(Iron_deadline_expired(d));
    TEST_ASSERT_EQUAL_INT(0, Iron_deadline_remaining_ms(d));
}

void test_deadline_from_5000ms(void) {
    Iron_Deadline d = Iron_deadline_from_timeout_ms(5000);
    int rem = Iron_deadline_remaining_ms(d);
    TEST_ASSERT_TRUE_MESSAGE(rem > 0,    "remaining should be > 0");
    TEST_ASSERT_TRUE_MESSAGE(rem <= 5000, "remaining should be <= 5000");
    TEST_ASSERT_FALSE(Iron_deadline_expired(d));
}

void test_deadline_huge_timeout(void) {
    Iron_Deadline d = Iron_deadline_from_timeout_ms(INT64_MAX);
    int rem = Iron_deadline_remaining_ms(d);
    TEST_ASSERT_EQUAL_INT(0x7FFFFFFF, rem);
    TEST_ASSERT_FALSE(Iron_deadline_expired(d));
}

void test_deadline_already_passed(void) {
    /* Build a deadline in the distant past by hand. */
    Iron_Deadline d;
    d.deadline_mono_ms = 1;  /* essentially epoch start — monotonic clock is now FAR ahead */
    TEST_ASSERT_EQUAL_INT(0, Iron_deadline_remaining_ms(d));
    TEST_ASSERT_TRUE(Iron_deadline_expired(d));
}

void test_cond_timedwait_100ms(void) {
    iron_mutex_t lock;
    iron_cond_t  cv;
    IRON_MUTEX_INIT(lock);
    IRON_COND_INIT(cv);

    IRON_MUTEX_LOCK(lock);
    uint64_t t0 = Iron_monotonic_now_ms();
    int rc = iron_cond_timedwait_ms(&cv, &lock, 100);
    uint64_t t1 = Iron_monotonic_now_ms();
    IRON_MUTEX_UNLOCK(lock);

    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_TIMEDWAIT_EXPIRED, rc,
        "expected timeout return from 100ms wait with no signaler");

    uint64_t elapsed = t1 - t0;
    TEST_ASSERT_TRUE_MESSAGE(elapsed >= 50,  "wait returned too early (< 50ms)");
    TEST_ASSERT_TRUE_MESSAGE(elapsed <= 400, "wait returned too late (> 400ms)");

    IRON_MUTEX_DESTROY(lock);
    IRON_COND_DESTROY(cv);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_monotonic_non_decreasing);
    RUN_TEST(test_deadline_from_zero_timeout);
    RUN_TEST(test_deadline_from_negative_timeout);
    RUN_TEST(test_deadline_from_5000ms);
    RUN_TEST(test_deadline_huge_timeout);
    RUN_TEST(test_deadline_already_passed);
    RUN_TEST(test_cond_timedwait_100ms);
    return UNITY_END();
}
