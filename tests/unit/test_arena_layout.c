/* test_arena_layout.c — Phase 28 GA1 arena allocation ABI layout invariants.
 *
 * Wave 0 RED for Plan 28-01 Task 1; flips GREEN as soon as Plan 28-02 lands
 * src/runtime/iron_arena_rt.h defining IronArenaAllocHdr (16B: arena_gen@0,
 * size@8) and Iron_ArenaSave ({ offset, gen_snapshot } — two uint64 on LP64).
 *
 * What this test locks (28-CONTEXT.md GA1 + 28-RESEARCH.md §A):
 *   - sizeof(IronArenaAllocHdr) == 16  (minimal per-allocation prefix header —
 *     arena back-ref + size — smaller than the full Phase-19 IronAllocHdr).
 *   - offsetof(arena_gen) == 0  (deref routing reads the back-ref first)
 *   - offsetof(size)      == 8  (destructor-walk + accounting)
 *   - sizeof(Iron_ArenaSave) covers two uint64 (== 16 on LP64): { offset,
 *     gen_snapshot } — save() captures bump offset + generation snapshot.
 *
 * Mirrors tests/unit/test_weak_rc_layout.c structure exactly (the Phase 27 ABI
 * `_Static_assert` mirror pattern). The compile-time `_Static_assert`s are the
 * hard ABI lock; the runtime Unity tests are the registration witnesses ctest
 * enumerates under phase28-invariant.
 *
 * RED note: this TU `#include`s "runtime/iron_arena_rt.h" which does NOT exist
 * until Plan 28-02. The CMake header-existence guard prevents this file from
 * breaking configure; once the header lands the guard is removed and the
 * `_Static_assert`s become the binding ABI lock.
 */

#include "unity.h"
#include "runtime/iron_arena_rt.h"

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Compile-time ABI lock (28-RESEARCH.md §A) ─────────────────────────── */

_Static_assert(sizeof(IronArenaAllocHdr) == 16,
               "ARENA-LAYOUT ABI lock — IronArenaAllocHdr must be 16B "
               "(arena_gen@0 + size@8). See 28-RESEARCH.md §A.");
_Static_assert(offsetof(IronArenaAllocHdr, arena_gen) == 0,
               "arena_gen@0 — deref-routing back-reference is the first field");
_Static_assert(offsetof(IronArenaAllocHdr, size) == 8,
               "size@8 — destructor-walk + arena accounting");

/* ── Test A — IronArenaAllocHdr sizeof == 16 ───────────────────────────── */

void test_arena_alloc_hdr_size_16(void) {
    /* Phase 28 GA1 ABI lock: minimal per-allocation prefix header. */
    TEST_ASSERT_EQUAL_UINT64(16, (uint64_t)sizeof(IronArenaAllocHdr));
}

/* ── Test B — explicit offsetof asserts for both fields ────────────────── */

void test_arena_alloc_hdr_offsets(void) {
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)offsetof(IronArenaAllocHdr, arena_gen));
    TEST_ASSERT_EQUAL_UINT64(8, (uint64_t)offsetof(IronArenaAllocHdr, size));
}

/* ── Test C — Iron_ArenaSave is two uint64 (LP64) ──────────────────────── */

void test_arena_save_covers_two_uint64(void) {
    /* ArenaSave = { offset, gen_snapshot } — save() captures the current
     * bump offset + the arena generation snapshot (28-CONTEXT.md GA1). On
     * LP64 with two uint64 fields the struct is exactly 16B. */
    TEST_ASSERT_EQUAL_UINT64(16, (uint64_t)sizeof(Iron_ArenaSave));
    TEST_ASSERT_TRUE(sizeof(Iron_ArenaSave) >= 2 * sizeof(uint64_t));
}

/* ── Unity entrypoint ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arena_alloc_hdr_size_16);
    RUN_TEST(test_arena_alloc_hdr_offsets);
    RUN_TEST(test_arena_save_covers_two_uint64);
    return UNITY_END();
}
