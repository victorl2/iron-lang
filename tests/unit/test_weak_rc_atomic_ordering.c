/* test_weak_rc_atomic_ordering.c — Phase 27 GA1 single-thread atomic ordering.
 *
 * Wave 0 RED for Plan 27-01 Task 2; turns GREEN once iron_weak_rc_retain,
 * iron_weak_rc_release, and the extended iron_rc_release block-free guard
 * are in place.
 *
 * Multi-thread stress lives in test_runtime_weak_rc_concurrent.c (Linux+TSan
 * gated). The mid-destructor race coverage lives in test_weak_rc_upgrade_race.c
 * (Plan 27-01 Task 3).
 *
 * What this test locks (CONTEXT.md GA1):
 *   - weak_count monotonic via iron_weak_rc_retain (relaxed inc) — N retains
 *     plus N releases leaves the count at its starting value.
 *   - iron_weak_rc_release with strong-alive and weak-only-self does NOT
 *     free the block (acquire-load on refcount observes strong > 0).
 *   - iron_rc_release with weak_count > 0 invokes drop_fn but leaves the
 *     block allocated (deferred-free path — header persists until weak=0).
 *   - The final iron_weak_rc_release after a strong=0 transition completes
 *     the deferred free (refcount==0 observed via acquire-load).
 *
 * Task 3 will replace the manual weak_count increments here with calls to
 * iron_rc_downgrade once that helper exists. For Task 2 we exercise the
 * runtime helpers we just introduced and use IRON_ATOMIC_U64_FETCH_ADD_RELAXED
 * directly to simulate the eventual downgrade behaviour.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Probe drop_fn ─────────────────────────────────────────────────────── */

static int g_drop_calls = 0;

static void probe_drop(void *self) {
    (void)self;
    g_drop_calls++;
}

/* ── Test 1 — weak retain/release balance leaves count at start ───────── */

void test_weak_rc_retain_release_balance(void) {
    /* 100 weak retains + 100 weak releases on an rc that ALSO keeps a strong
     * holder alive (so the block is never freed); weak_count returns to its
     * starting value (1 — the strong cohort's collective weak, Rust Arc
     * scheme; see iron_rc_alloc). */
    void *p = iron_rc_alloc(sizeof(int), NULL);
    TEST_ASSERT_NOT_NULL(p);

    Iron_RcHeader *rch = iron_rc_header_of(p);
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    for (int i = 0; i < 100; i++) {
        iron_weak_rc_retain(p);
    }
    TEST_ASSERT_EQUAL_UINT64(101, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    for (int i = 0; i < 100; i++) {
        iron_weak_rc_release(p);
    }
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    /* Strong still 1. Final release frees the block (weak now 0). */
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    iron_rc_release(p);
}

/* ── Test 2 — weak_release with strong alive does NOT free the block ──── */

void test_weak_release_does_not_free_when_strong_alive(void) {
    /* Mirror image of Pitfall 2 from RESEARCH.md §7 — the LAST weak going
     * to 0 must NOT free the block while the strong holder is alive. */
    g_drop_calls = 0;
    void *p = iron_rc_alloc(sizeof(int64_t), probe_drop);
    TEST_ASSERT_NOT_NULL(p);

    Iron_RcHeader *rch = iron_rc_header_of(p);
    *(int64_t *)p = 0xCAFEFACEull;

    /* Bump weak, then release. weak transitions 1 -> 2 -> 1 around the
     * collective-weak baseline of 1 (strong still alive). */
    iron_weak_rc_retain(p);
    TEST_ASSERT_EQUAL_UINT64(2, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));
    iron_weak_rc_release(p);

    /* If the block had been wrongly freed, this load would be UAF (caught
     * by ASan under the silvaserver podman setup). Asserting both that the
     * refcount is still 1 AND the payload is still readable. */
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));
    TEST_ASSERT_EQUAL_INT64(0xCAFEFACEull, *(int64_t *)p);
    TEST_ASSERT_EQUAL_INT(0, g_drop_calls);

    /* Now release the strong — drop fires, weak=0 already so block freed. */
    iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(1, g_drop_calls);
}

/* ── Test 3 — strong_release with weak alive defers block free ────────── */

void test_strong_release_does_not_free_when_weak_alive(void) {
    /* Pitfall 1 reverse direction. When the LAST strong goes to 0 with
     * weak_count > 0, drop_fn must fire but the block must stay allocated
     * (the header keeps weak holders observing the now-dead payload).
     * The trailing iron_weak_rc_release completes the deferred free.
     *
     * Verification strategy (no extra TLS hooks available): observe the
     * pre-free state via the header (refcount must be 0, weak_count > 0),
     * confirm drop_fn fired exactly once, then release the weak which
     * completes the free. ASan in the podman container catches any
     * accidental UAF; double-free would also abort ASan.
     */
    g_drop_calls = 0;
    void *p = iron_rc_alloc(sizeof(int64_t), probe_drop);
    TEST_ASSERT_NOT_NULL(p);

    Iron_RcHeader *rch = iron_rc_header_of(p);

    /* Snapshot the rch pointer for post-strong-drop probing (the block
     * is intentionally NOT freed when strong hits 0 with weak alive).
     * weak_count = collective weak (1) + this real weak = 2. */
    iron_weak_rc_retain(p);
    TEST_ASSERT_EQUAL_UINT64(2, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    /* Release the strong. drop_fn must fire (payload destructor runs);
     * the block stays alive because weak_count > 0. */
    iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(1, g_drop_calls);

    /* Block is still alive — probe the header. refcount is now 0; weak
     * still 1. */
    TEST_ASSERT_EQUAL_UINT64(0, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    /* Now the last weak goes to 0 with strong already at 0; this is the
     * transition that completes the deferred free. */
    iron_weak_rc_release(p);

    /* drop_fn must NOT have fired a second time (weak release doesn't
     * invoke user destructors — only iron_rc_release does). */
    TEST_ASSERT_EQUAL_INT(1, g_drop_calls);
}

/* ── Unity entrypoint ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_weak_rc_retain_release_balance);
    RUN_TEST(test_weak_release_does_not_free_when_strong_alive);
    RUN_TEST(test_strong_release_does_not_free_when_weak_alive);
    return UNITY_END();
}
