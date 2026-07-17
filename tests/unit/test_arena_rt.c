/* test_arena_rt.c — Phase 28 arena runtime API happy-path coverage.
 *
 * Wave 0 RED for Plan 28-01 Task 1; flips GREEN as soon as Plan 28-02 lands
 * src/runtime/iron_arena_rt.{c,h} implementing the bump allocator API from
 * 28-RESEARCH.md §A. The signatures here are the contract Plan 28-02 fills:
 *
 *   Iron_Arena_RT *iron_arena_rt_new(uint64_t capacity, bool threadsafe,
 *                                 const char *name);
 *   Iron_FatPtr    iron_arena_rt_alloc(Iron_Arena_RT *a, uint64_t size);
 *   void           iron_arena_rt_reset(Iron_Arena_RT *a);            // ARENA-06
 *   Iron_ArenaSave iron_arena_rt_save(Iron_Arena_RT *a);             // ARENA-01
 *   void           iron_arena_rt_restore(Iron_Arena_RT *a, Iron_ArenaSave s); // ARENA-07
 *   uint64_t       iron_arena_rt_used(Iron_Arena_RT *a);             // ARENA-01
 *   uint64_t       iron_arena_rt_capacity(Iron_Arena_RT *a);         // ARENA-01
 *   void           iron_arena_rt_destroy(Iron_Arena_RT *a);
 *
 * Coverage (ARENA-01/05/06/07 + threadsafe variant ARENA-02):
 *   - new → used==0, capacity==requested
 *   - alloc bumps used by >= size (header included); distinct addresses
 *   - reset zeroes used (ARENA-06)
 *   - save/restore returns the bump offset to the save point (ARENA-07)
 *   - threadsafe-variant single-thread determinism: monotonic offsets, no
 *     overlap (true contention is the documented manual-only ARENA-02 check).
 *
 * RED note: `#include "runtime/iron_arena_rt.h"` does not exist until Plan
 * 28-02. The CMake header-existence guard keeps configure green meanwhile.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_arena_rt.h"

#include <stdbool.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── ARENA-01 — new reports requested capacity and zero used ───────────── */

void test_arena_new_used_capacity(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "t");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT64(4096, iron_arena_rt_capacity(a));
    TEST_ASSERT_EQUAL_UINT64(0, iron_arena_rt_used(a));
    iron_arena_rt_destroy(a);
}

/* ── ARENA-01 — alloc bumps used and returns distinct addresses ────────── */

void test_arena_alloc_bumps(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "t");
    TEST_ASSERT_NOT_NULL(a);

    uint64_t u0 = iron_arena_rt_used(a);
    Iron_FatPtr p1 = iron_arena_rt_alloc(a, 64);
    uint64_t u1 = iron_arena_rt_used(a);
    Iron_FatPtr p2 = iron_arena_rt_alloc(a, 64);
    uint64_t u2 = iron_arena_rt_used(a);

    /* Each alloc grows used by at least the payload size (header included). */
    TEST_ASSERT_TRUE(u1 - u0 >= 64);
    TEST_ASSERT_TRUE(u2 - u1 >= 64);
    TEST_ASSERT_NOT_NULL(p1.addr);
    TEST_ASSERT_NOT_NULL(p2.addr);
    TEST_ASSERT_TRUE(p1.addr != p2.addr);

    iron_arena_rt_destroy(a);
}

/* ── ARENA-06 — reset zeroes used ──────────────────────────────────────── */

void test_arena_reset_zeroes_used(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "t");
    TEST_ASSERT_NOT_NULL(a);

    (void)iron_arena_rt_alloc(a, 128);
    TEST_ASSERT_TRUE(iron_arena_rt_used(a) > 0);

    iron_arena_rt_reset(a);
    TEST_ASSERT_EQUAL_UINT64(0, iron_arena_rt_used(a));

    iron_arena_rt_destroy(a);
}

/* ── ARENA-07 — save/restore returns the bump offset to the save point ─── */

void test_arena_save_restore_offset(void) {
    Iron_Arena_RT *a = iron_arena_rt_new(4096, false, "t");
    TEST_ASSERT_NOT_NULL(a);

    (void)iron_arena_rt_alloc(a, 64);
    uint64_t o1 = iron_arena_rt_used(a);
    Iron_ArenaSave save = iron_arena_rt_save(a);

    (void)iron_arena_rt_alloc(a, 64);
    (void)iron_arena_rt_alloc(a, 64);
    TEST_ASSERT_TRUE(iron_arena_rt_used(a) > o1);

    iron_arena_rt_restore(a, save);
    TEST_ASSERT_EQUAL_UINT64(o1, iron_arena_rt_used(a));

    iron_arena_rt_destroy(a);
}

/* ── ARENA-02 — threadsafe variant single-thread determinism ───────────── */

void test_arena_threadsafe_variant(void) {
    /* Single-thread determinism for the atomic bump path: N allocations
     * yield monotonically increasing offsets with no overlap. True
     * multi-thread contention is the documented manual-only ARENA-02
     * check (see 28-RESEARCH.md §A); here we lock that the threadsafe
     * flag selects a path that is still correct under single-thread use. */
    Iron_Arena_RT *a = iron_arena_rt_new(1 << 20, true, "ts");
    TEST_ASSERT_NOT_NULL(a);

    const int N = 1000;
    uint64_t prev_used = iron_arena_rt_used(a);
    char *prev_addr = NULL;
    for (int i = 0; i < N; i++) {
        Iron_FatPtr fp = iron_arena_rt_alloc(a, 8);
        TEST_ASSERT_NOT_NULL(fp.addr);
        uint64_t cur_used = iron_arena_rt_used(a);
        /* Monotonic: used strictly grows each alloc. */
        TEST_ASSERT_TRUE(cur_used > prev_used);
        /* No overlap: addresses advance. */
        if (prev_addr) {
            TEST_ASSERT_TRUE((char *)fp.addr > prev_addr);
        }
        prev_used = cur_used;
        prev_addr = (char *)fp.addr;
    }
    /* used accounts for N allocations of (hdr + 8) each. */
    TEST_ASSERT_TRUE(iron_arena_rt_used(a) >= (uint64_t)N * 8);

    iron_arena_rt_destroy(a);
}

/* ── Unity entrypoint ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arena_new_used_capacity);
    RUN_TEST(test_arena_alloc_bumps);
    RUN_TEST(test_arena_reset_zeroes_used);
    RUN_TEST(test_arena_save_restore_offset);
    RUN_TEST(test_arena_threadsafe_variant);
    return UNITY_END();
}
