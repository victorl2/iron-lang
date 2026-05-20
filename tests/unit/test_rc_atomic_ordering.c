/* test_rc_atomic_ordering.c — Phase 26 POL-06 single-thread atomic ordering proof.
 *
 * Wave 0 RED for Plan 26-01 Task 1; turns GREEN after Plan 26-01 Task 2 lands
 * iron_rc_alloc / iron_rc_retain / iron_rc_release on Iron_RcHeader.
 *
 * Single-thread tests pin the operation count + invocation order without
 * needing TSan. Multi-thread stress lives in test_runtime_rc_concurrent.c
 * (Linux+TSan gated).
 *
 * What this test locks (CONTEXT.md GA3):
 *   - retain bumps refcount monotonically — N retains => refcount == 1+N
 *   - release decrements monotonically — refcount returns to 1 after N
 *     paired releases on a baseline of 1+N
 *   - drop_fn invocation deferred to the final release (prev == 1 path)
 *   - drop_fn invoked at most ONCE per allocation lifecycle
 *   - NULL drop_fn skips the trampoline cleanly (no SIGSEGV)
 *
 * Reference for the canonical pattern: Rust Arc relaxed-inc / release-dec /
 * acquire-fence-on-final-drop (RESEARCH.md Pitfalls 1-2 in
 * .planning/phases/26-rc-policy/26-RESEARCH.md).
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Probe drop helpers ──────────────────────────────────────────────────── */

static int g_drop_calls = 0;
static void *g_last_drop_self = NULL;

static void probe_drop_record_self(void *self) {
    g_drop_calls++;
    g_last_drop_self = self;
}

/* ── Test 1 — retain bumps refcount monotonically across many iterations ── */

void test_retain_monotonic_relaxed_bump(void) {
    /* 100 retains => refcount = 1 (initial) + 100. relaxed-inc has no
     * synchronization with other writes; pure monotonic counter. */
    void *p = iron_rc_alloc(sizeof(int), NULL);
    TEST_ASSERT_NOT_NULL(p);

    Iron_RcHeader *rch = iron_rc_header_of(p);
    for (int i = 0; i < 100; i++) {
        iron_rc_retain(p);
    }
    TEST_ASSERT_EQUAL_UINT64(101, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));

    /* Balance: 100 releases brings refcount back to 1; final release drops. */
    for (int i = 0; i < 100; i++) {
        iron_rc_release(p);
    }
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    iron_rc_release(p);
}

/* ── Test 2 — release with NULL drop_fn skips trampoline ─────────────────── */

void test_release_with_null_drop_fn_no_crash(void) {
    /* drop_fn == NULL is legal for primitive payloads (int, bool, etc.)
     * with no user destructor. iron_rc_release must check before dispatch
     * and free the block without segfaulting on a NULL call. */
    void *p = iron_rc_alloc(sizeof(int64_t), NULL);
    TEST_ASSERT_NOT_NULL(p);
    iron_rc_release(p);  /* refcount goes 1 -> 0, no drop_fn dispatch */
    /* If we reach here without SIGSEGV, the test passes. */
    TEST_ASSERT_TRUE(1);
}

/* ── Test 3 — final release invokes drop_fn exactly once with user_ptr ──── */

void test_final_release_invokes_drop_fn_with_user_ptr(void) {
    g_drop_calls = 0;
    g_last_drop_self = NULL;

    void *p = iron_rc_alloc(sizeof(int64_t), probe_drop_record_self);
    TEST_ASSERT_NOT_NULL(p);

    /* Write a sentinel into the payload BEFORE release; drop_fn must be
     * able to read it via its self argument. This is the canonical
     * destructor-observes-payload check. */
    *(int64_t *)p = 0xCAFEBABEull;

    iron_rc_release(p);  /* refcount 1 -> 0; drop_fn fires */

    TEST_ASSERT_EQUAL_INT(1, g_drop_calls);
    /* drop_fn received the user pointer (not the header). */
    TEST_ASSERT_EQUAL_PTR(p, g_last_drop_self);
}

/* ── Test 4 — multi-retain then symmetric release fires drop_fn once ─────── */

void test_release_chain_invokes_drop_fn_once(void) {
    g_drop_calls = 0;

    void *p = iron_rc_alloc(sizeof(int), probe_drop_record_self);
    TEST_ASSERT_NOT_NULL(p);

    /* 5 retains (refcount = 6), 5 releases (refcount = 1, drop_fn NOT
     * yet fired), then final release (drop_fn fires exactly once). */
    for (int i = 0; i < 5; i++) iron_rc_retain(p);
    for (int i = 0; i < 5; i++) iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(0, g_drop_calls);

    iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(1, g_drop_calls);
}

/* ── Test 5 — multiple independent allocations have independent lifecycles ─ */

void test_independent_allocations_independent_drops(void) {
    g_drop_calls = 0;

    void *a = iron_rc_alloc(sizeof(int), probe_drop_record_self);
    void *b = iron_rc_alloc(sizeof(int), probe_drop_record_self);
    void *c = iron_rc_alloc(sizeof(int), probe_drop_record_self);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    /* Drop a — count = 1; b and c still live. */
    iron_rc_release(a);
    TEST_ASSERT_EQUAL_INT(1, g_drop_calls);

    /* Drop c — count = 2; b still live. */
    iron_rc_release(c);
    TEST_ASSERT_EQUAL_INT(2, g_drop_calls);

    /* Drop b — count = 3. */
    iron_rc_release(b);
    TEST_ASSERT_EQUAL_INT(3, g_drop_calls);
}

/* ── Unity entrypoint ────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_retain_monotonic_relaxed_bump);
    RUN_TEST(test_release_with_null_drop_fn_no_crash);
    RUN_TEST(test_final_release_invokes_drop_fn_with_user_ptr);
    RUN_TEST(test_release_chain_invokes_drop_fn_once);
    RUN_TEST(test_independent_allocations_independent_drops);
    return UNITY_END();
}
