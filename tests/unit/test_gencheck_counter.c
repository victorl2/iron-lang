/* test_gencheck_counter.c — Phase 30 Plan 30-01 (OPT-08 input): opt-in
 * generation-check counter.
 *
 * This test is built WITH -DIRON_GENCHECK_COUNT (set via
 * target_compile_definitions in tests/unit/CMakeLists.txt), turning ON the
 * three `_Atomic uint64_t` heap/stack/arena counters that
 * src/runtime/iron_gencheck_count.c compiles out by default and that the
 * static-inline iron_check_*_pointer_gen guards in iron_runtime.h bump. It
 * asserts the counters increment exactly once per check call so the deferred
 * OPT-08 published elision-rate report (Phase 36) has a verified data source.
 * The counter is OFF in normal builds (no atomic in the hot path); this test
 * exists solely to prove the instrumented path works.
 *
 * The stack check (iron_check_stack_pointer_gen) is exercised because it needs
 * only the TLS iron_stack_gen counter — no IronAllocHdr / arena header to fake.
 * A fat pointer whose .gen == iron_stack_gen and .addr != NULL passes the check
 * (no panic) while still bumping the stack tally.
 *
 * Oracle: N stack checks → iron_gencheck_counts() reports stack == N.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdint.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* Build a fat pointer that passes the stack check (gen matches the live TLS
 * frame counter, addr is a real local). */
static Iron_FatPtr fresh_stack_fp(void *addr) {
    Iron_FatPtr fp;
    fp.addr = addr;
    fp.gen  = iron_stack_gen;
    return fp;
}

void test_counter_starts_at_zero(void) {
    iron_gencheck_counts_reset();
    uint64_t heap = 1, stack = 1, arena = 1;
    iron_gencheck_counts(&heap, &stack, &arena);
    TEST_ASSERT_EQUAL_UINT64(0, heap);
    TEST_ASSERT_EQUAL_UINT64(0, stack);
    TEST_ASSERT_EQUAL_UINT64(0, arena);
}

void test_counter_counts_stack_checks(void) {
    iron_gencheck_counts_reset();
    int local = 42;
    Iron_FatPtr fp = fresh_stack_fp(&local);

    iron_check_stack_pointer_gen(fp, "test.iron", 1);
    iron_check_stack_pointer_gen(fp, "test.iron", 2);
    iron_check_stack_pointer_gen(fp, "test.iron", 3);

    uint64_t heap = 0, stack = 0, arena = 0;
    iron_gencheck_counts(&heap, &stack, &arena);
    TEST_ASSERT_EQUAL_UINT64(3, stack);
    /* Heap/arena guards never ran. */
    TEST_ASSERT_EQUAL_UINT64(0, heap);
    TEST_ASSERT_EQUAL_UINT64(0, arena);
}

void test_counter_reset_zeroes(void) {
    iron_gencheck_counts_reset();
    int local = 7;
    Iron_FatPtr fp = fresh_stack_fp(&local);
    iron_check_stack_pointer_gen(fp, "test.iron", 1);

    uint64_t stack = 0;
    iron_gencheck_counts(NULL, &stack, NULL);
    TEST_ASSERT_EQUAL_UINT64(1, stack);

    iron_gencheck_counts_reset();
    stack = 99;
    iron_gencheck_counts(NULL, &stack, NULL);
    TEST_ASSERT_EQUAL_UINT64(0, stack);
}

void test_counter_null_args_safe(void) {
    iron_gencheck_counts_reset();
    int local = 5;
    Iron_FatPtr fp = fresh_stack_fp(&local);
    iron_check_stack_pointer_gen(fp, "test.iron", 1);

    /* NULL out-params must not crash — accessor tolerates partial reads. */
    iron_gencheck_counts(NULL, NULL, NULL);
    uint64_t stack = 0;
    iron_gencheck_counts(NULL, &stack, NULL);
    TEST_ASSERT_EQUAL_UINT64(1, stack);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_counter_starts_at_zero);
    RUN_TEST(test_counter_counts_stack_checks);
    RUN_TEST(test_counter_reset_zeroes);
    RUN_TEST(test_counter_null_args_safe);
    return UNITY_END();
}
