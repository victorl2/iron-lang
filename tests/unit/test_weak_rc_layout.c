/* test_weak_rc_layout.c — Phase 27 GA1 weak rc layout invariants.
 *
 * Wave 0 RED for Plan 27-01 Task 1; turns GREEN as soon as Iron_RcHeader is
 * extended from 16B to 24B with the new weak_count field at offset 16 and
 * iron_rc_alloc initializes the field to 0.
 *
 * What this test locks (CONTEXT.md GA1):
 *   - sizeof(Iron_RcHeader) == 24 (Phase 27 ABI re-lock)
 *   - offsetof(refcount)   == 0   (hot-path retain/release; Phase 26 frozen)
 *   - offsetof(drop_fn)    == 8   (cold-path final drop; Phase 26 frozen)
 *   - offsetof(weak_count) == 16  (Phase 27 GA1 append)
 *   - Initial weak_count == 0 on every fresh iron_rc_alloc (Pitfall 4 —
 *     uninitialised garbage would break the deferred-free condition).
 *   - Initialization works identically for primitive (NULL drop_fn) and
 *     user-destructor (non-NULL drop_fn) payloads.
 *
 * The probe_drop counter approach mirrors test_rc_layout.c — drop_fn fires
 * BEFORE the block is freed, so the captured side-effect is observable
 * post-release without touching released memory.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Probe drop_fn ─────────────────────────────────────────────────────── */

static int g_probe_drop_count = 0;

static void probe_drop(void *self) {
    (void)self;
    g_probe_drop_count++;
}

/* ── Test A — Iron_RcHeader sizeof == 24 ──────────────────────────────── */

void test_iron_rc_header_size_24(void) {
    /* Phase 27 GA1 ABI re-lock: 24B (refcount@0 + drop_fn@8 + weak_count@16). */
    TEST_ASSERT_EQUAL_UINT64(24, (uint64_t)sizeof(Iron_RcHeader));
}

/* ── Test B — explicit offsetof asserts for all 3 fields ──────────────── */

void test_iron_rc_header_offsets(void) {
    TEST_ASSERT_EQUAL_UINT64(0,  (uint64_t)offsetof(Iron_RcHeader, refcount));
    TEST_ASSERT_EQUAL_UINT64(8,  (uint64_t)offsetof(Iron_RcHeader, drop_fn));
    TEST_ASSERT_EQUAL_UINT64(16, (uint64_t)offsetof(Iron_RcHeader, weak_count));
}

/* ── Test C — initial weak_count == 0 with NULL drop_fn ───────────────── */

void test_weak_count_initial_zero_after_alloc(void) {
    /* Pitfall 4: an uninitialised weak_count would observe garbage on the
     * first iron_rc_downgrade and prevent the deferred-free path from ever
     * triggering. iron_rc_alloc MUST zero the field. */
    void *p = iron_rc_alloc(sizeof(int), NULL);
    TEST_ASSERT_NOT_NULL(p);

    Iron_RcHeader *rch = iron_rc_header_of(p);
    TEST_ASSERT_NOT_NULL(rch);
    TEST_ASSERT_EQUAL_UINT64(0,
        IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    iron_rc_release(p);  /* refcount 1 -> 0; weak_count still 0; block freed */
}

/* ── Test D — initial weak_count == 0 with non-NULL drop_fn ───────────── */

void test_weak_count_initial_zero_with_drop_fn(void) {
    /* Same invariant as Test C, but with a non-NULL drop_fn to prove the
     * field still initialises cleanly when the destructor slot is wired. */
    g_probe_drop_count = 0;
    void *p = iron_rc_alloc(sizeof(int64_t), probe_drop);
    TEST_ASSERT_NOT_NULL(p);

    Iron_RcHeader *rch = iron_rc_header_of(p);
    TEST_ASSERT_NOT_NULL(rch);
    TEST_ASSERT_EQUAL_UINT64(0,
        IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));
    TEST_ASSERT_EQUAL_UINT64(1,
        IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));

    iron_rc_release(p);  /* drop_fn fires once; block freed */
    TEST_ASSERT_EQUAL_INT(1, g_probe_drop_count);
}

/* ── Unity entrypoint ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_rc_header_size_24);
    RUN_TEST(test_iron_rc_header_offsets);
    RUN_TEST(test_weak_count_initial_zero_after_alloc);
    RUN_TEST(test_weak_count_initial_zero_with_drop_fn);
    return UNITY_END();
}
