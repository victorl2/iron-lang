/* test_rc_layout.c — Phase 26 POL-06 + Phase 27 GA1 ABI layout invariants.
 *
 * Originally Wave 0 RED for Plan 26-01 Task 1; turned GREEN at Plan 26-01
 * Task 2 with the 16B Iron_RcHeader. Plan 27-01 Task 1 re-locks at 24B
 * (Phase 27 appended weak_count at offset 16 per CONTEXT.md GA1).
 *
 * Pattern reference: tests/unit/test_runtime_heap_track.c + Phase 19
 * sizeof+offsetof + initial-state probes. Wraps each assertion in its own
 * Unity test for granular RED reporting.
 *
 * What this test locks (Phase 26 GA1 + Phase 27 GA1):
 *   - sizeof(Iron_RcHeader) == 24  (Phase 27 ABI re-lock; also
 *     _Static_assert in src/runtime/iron_runtime.h)
 *   - offsetof(refcount)   == 0    (hot path; relaxed-inc on retain;
 *     ABI-frozen Phase 26)
 *   - offsetof(drop_fn)    == 8    (cold path; final-drop call site;
 *     ABI-frozen Phase 26)
 *   - offsetof(weak_count) == 16   (Phase 27 GA1 append; relaxed inc/dec)
 *   - iron_rc_alloc(size, drop_fn) returns non-NULL user pointer
 *   - Initial refcount == 1 (visible via iron_rc_header_of acquire load)
 *   - Balanced retain/release pairs leave refcount monotonic with no
 *     premature drop_fn invocation; final release fires drop_fn once.
 *
 * The probe_drop counter approach (drop_fn increments a static int) avoids
 * use-after-free reads of the freed block: drop_fn runs BEFORE free(block),
 * so the captured counter value is observable post-release.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Probe drop_fn — increments a static counter for invocation accounting ─ */

static int g_probe_drop_count = 0;

static void probe_drop(void *self) {
    (void)self;
    g_probe_drop_count++;
}

/* ── Test A — ABI layout invariants (size + offsets) ─────────────────────── */

void test_iron_rc_header_size_24_bytes(void) {
    /* Phase 27 GA1 ABI re-lock: 24B on 64-bit POSIX/Win32. Locked at TU
     * scope in iron_runtime.h via _Static_assert; this runtime probe
     * catches any accidental layout change that slips past the compile-
     * time guard (e.g., #pragma pack manipulation in a downstream TU). */
    TEST_ASSERT_EQUAL_UINT64(24, (uint64_t)sizeof(Iron_RcHeader));
}

void test_iron_rc_header_refcount_at_offset_0(void) {
    /* refcount must be the FIRST field — hot-path retain/release walks
     * the user pointer back by IronAllocHdr + Iron_RcHeader and expects
     * the refcount to sit at the block prefix. ABI-frozen Phase 26. */
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)offsetof(Iron_RcHeader, refcount));
}

void test_iron_rc_header_drop_fn_at_offset_8(void) {
    /* drop_fn at offset 8 — cold path; only loaded on final drop.
     * ABI-frozen Phase 26. */
    TEST_ASSERT_EQUAL_UINT64(8, (uint64_t)offsetof(Iron_RcHeader, drop_fn));
}

void test_iron_rc_header_weak_count_at_offset_16(void) {
    /* weak_count at offset 16 — Phase 27 GA1 ABI lock. Starts at 1 (the
     * strong cohort's collective weak); the weak counter's 1→0 edge is the
     * single block-free linearization point (Rust Arc scheme). */
    TEST_ASSERT_EQUAL_UINT64(16, (uint64_t)offsetof(Iron_RcHeader, weak_count));
}

/* ── Test B — alloc returns non-NULL with initial refcount == 1 ──────────── */

void test_iron_rc_alloc_returns_non_null(void) {
    void *p = iron_rc_alloc(sizeof(int), NULL);
    TEST_ASSERT_NOT_NULL(p);
    iron_rc_release(p);
}

void test_iron_rc_alloc_initial_refcount_one(void) {
    void *p = iron_rc_alloc(sizeof(int), NULL);
    TEST_ASSERT_NOT_NULL(p);
    Iron_RcHeader *rch = iron_rc_header_of(p);
    TEST_ASSERT_NOT_NULL(rch);
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    iron_rc_release(p);
}

/* ── Test C — balanced retain/release leaves drop_fn unfired until last ──── */

void test_iron_rc_retain_release_balanced_one_drop(void) {
    g_probe_drop_count = 0;
    void *p = iron_rc_alloc(sizeof(int), probe_drop);
    TEST_ASSERT_NOT_NULL(p);

    /* retain x2 → refcount = 3 */
    iron_rc_retain(p);
    iron_rc_retain(p);
    Iron_RcHeader *rch = iron_rc_header_of(p);
    TEST_ASSERT_EQUAL_UINT64(3, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));

    /* release x2 → refcount = 1, drop_fn NOT yet invoked */
    iron_rc_release(p);
    iron_rc_release(p);
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_INT(0, g_probe_drop_count);

    /* final release → refcount = 0, drop_fn invoked exactly once.
     * NOTE: cannot read rch->refcount post-release because the block is
     * freed; assert drop_fn invocation count instead. */
    iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(1, g_probe_drop_count);
}

/* ── Unity entrypoint ────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_rc_header_size_24_bytes);
    RUN_TEST(test_iron_rc_header_refcount_at_offset_0);
    RUN_TEST(test_iron_rc_header_drop_fn_at_offset_8);
    RUN_TEST(test_iron_rc_header_weak_count_at_offset_16);
    RUN_TEST(test_iron_rc_alloc_returns_non_null);
    RUN_TEST(test_iron_rc_alloc_initial_refcount_one);
    RUN_TEST(test_iron_rc_retain_release_balanced_one_drop);
    return UNITY_END();
}
