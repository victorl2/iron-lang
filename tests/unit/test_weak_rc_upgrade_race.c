/* test_weak_rc_upgrade_race.c — Phase 27 GA2 mid-destructor upgrade race.
 *
 * Wave 0 RED for Plan 27-01 Task 3; turns GREEN when iron_rc_downgrade +
 * iron_rc_upgrade land with the Rust Arc canonical acquire-load + relaxed
 * CAS loop semantics.
 *
 * What this test locks (CONTEXT.md GA2):
 *   - iron_rc_upgrade(NULL) returns NULL deterministically (weak rc null
 *     case from GA3).
 *   - iron_rc_upgrade on a still-alive strong returns the same user
 *     pointer AND bumps refcount by 1 (CAS-success path).
 *   - iron_rc_upgrade after the last strong has been released returns
 *     NULL (refcount observed at 0; payload destroyed but header alive
 *     via outstanding weak ref).
 *   - Mid-destructor race: 2 threads — A holds last strong + releases on
 *     signal; B busy-waits then attempts upgrade. Across 10000 iterations
 *     the upgrade NEVER reserves a strong ref to a destructed payload —
 *     every iteration is either an upgrade-before-release (success) or
 *     an upgrade-after-release (NULL). null_count + success_count must
 *     equal 10000 AND null_count >= 1 (proves the race window is
 *     exercised, not always-success or always-null).
 *
 * Pattern reference: tests/unit/test_runtime_rc_concurrent.c pthread
 * harness.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Drop counter for observability ────────────────────────────────────── */

static int g_race_drop_count = 0;

static void race_drop(void *self) {
    (void)self;
    g_race_drop_count++;
}

/* ── Test A — upgrade on NULL weak rc returns NULL ────────────────────── */

void test_upgrade_on_null_weak_returns_null(void) {
    TEST_ASSERT_NULL(iron_rc_upgrade(NULL));
}

/* ── Test B — upgrade while strong is still alive succeeds ────────────── */

void test_upgrade_while_strong_alive_succeeds(void) {
    g_race_drop_count = 0;
    void *p = iron_rc_alloc(sizeof(int64_t), race_drop);
    TEST_ASSERT_NOT_NULL(p);
    Iron_RcHeader *rch = iron_rc_header_of(p);

    /* Downgrade to obtain a weak handle; weak_count goes 1 -> 2 (baseline
     * 1 is the strong cohort's collective weak). */
    void *w = iron_rc_downgrade(p);
    TEST_ASSERT_EQUAL_PTR(p, w);
    TEST_ASSERT_EQUAL_UINT64(2, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));

    /* Upgrade — refcount goes 1 -> 2; returned ptr equals original. */
    void *up = iron_rc_upgrade(w);
    TEST_ASSERT_EQUAL_PTR(p, up);
    TEST_ASSERT_EQUAL_UINT64(2, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));

    /* Tear down: 2 strong releases (drop fires once on second), 1 weak. */
    iron_rc_release(up);
    iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(1, g_race_drop_count);
    iron_weak_rc_release(w);
}

/* ── Test C — upgrade after last strong released returns NULL ────────── */

void test_upgrade_after_strong_drop_returns_null(void) {
    g_race_drop_count = 0;
    void *p = iron_rc_alloc(sizeof(int64_t), race_drop);
    TEST_ASSERT_NOT_NULL(p);
    Iron_RcHeader *rch = iron_rc_header_of(p);

    void *w = iron_rc_downgrade(p);  /* weak_count 1 -> 2 (collective +1) */

    /* Release the last strong. drop_fn fires; header stays alive (weak>0). */
    iron_rc_release(p);
    TEST_ASSERT_EQUAL_INT(1, g_race_drop_count);
    TEST_ASSERT_EQUAL_UINT64(0, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount));
    TEST_ASSERT_EQUAL_UINT64(1, IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count));

    /* Upgrade now observes refcount==0; returns NULL. */
    void *up = iron_rc_upgrade(w);
    TEST_ASSERT_NULL(up);
    TEST_ASSERT_EQUAL_INT(1, g_race_drop_count);   /* no second drop */

    /* Last weak completes deferred free. */
    iron_weak_rc_release(w);
}

/* ── Test D — mid-destructor race: A drops last strong, B races to upgrade ─
 *
 * Per-iteration choreography:
 *   - Main allocates rc, downgrades to obtain weak handle.
 *   - main_release_pending atomic flag is initialised to false.
 *   - Thread A: spin-wait until main_release_pending becomes true; then
 *     iron_rc_release(p) on the last strong.
 *   - Thread B: spin-wait until main_release_pending becomes true; then
 *     iron_rc_upgrade(w) — observe the result (NULL or non-NULL).
 *   - Main flips main_release_pending = true → A and B race.
 *   - Main joins both threads, records B's result, cleans up.
 *
 * After 10000 iterations: null_count + success_count == 10000 (every
 * iteration produces exactly one observation) AND null_count >= 1
 * (the race window IS exercised, not always-success).
 *
 * Critical correctness: upgrade-success MUST imply the payload survives
 * — i.e., the runtime guarantees no UAF for the caller. Test asserts
 * this by reading the payload sentinel post-upgrade on the success path
 * (with a guarding strong release at the end).
 */

#define UPGRADE_RACE_ITERS 10000

static _Atomic int g_null_count_total = 0;
static _Atomic int g_success_count_total = 0;

typedef struct {
    void *strong;
    void *weak;
    _Atomic int signal;        /* 0 → wait; 1 → go */
    _Atomic int b_result_kind; /* 0 → not observed; 1 → NULL; 2 → success */
} race_ctx_t;

static void *race_thread_drop_strong(void *arg) {
    race_ctx_t *ctx = (race_ctx_t *)arg;
    while (atomic_load_explicit(&ctx->signal, memory_order_acquire) == 0) {
        /* busy-wait */
    }
    iron_rc_release(ctx->strong);
    return NULL;
}

static void *race_thread_attempt_upgrade(void *arg) {
    race_ctx_t *ctx = (race_ctx_t *)arg;
    while (atomic_load_explicit(&ctx->signal, memory_order_acquire) == 0) {
        /* busy-wait */
    }
    void *up = iron_rc_upgrade(ctx->weak);
    if (up == NULL) {
        atomic_store_explicit(&ctx->b_result_kind, 1, memory_order_release);
    } else {
        /* Read the payload sentinel — proves the upgrade didn't reserve a
         * ref to a destructed payload (else this would be UAF). The static
         * sentinel 0xDEADBEEF was written by main pre-race. */
        TEST_ASSERT_EQUAL_INT64((int64_t)0xDEADBEEFull, *(int64_t *)up);
        /* Release the upgraded strong; drop only fires when refcount returns
         * to 0 (which it must NOT here — A's release brought it to 1 once
         * B's upgrade succeeded; B's release brings it to 0 now). */
        iron_rc_release(up);
        atomic_store_explicit(&ctx->b_result_kind, 2, memory_order_release);
    }
    return NULL;
}

void test_upgrade_mid_destructor_race_returns_null_or_success(void) {
    atomic_store(&g_null_count_total, 0);
    atomic_store(&g_success_count_total, 0);

    int null_count = 0;
    int success_count = 0;

    for (int iter = 0; iter < UPGRADE_RACE_ITERS; iter++) {
        race_ctx_t ctx;
        ctx.strong = iron_rc_alloc(sizeof(int64_t), race_drop);
        TEST_ASSERT_NOT_NULL(ctx.strong);
        *(int64_t *)ctx.strong = (int64_t)0xDEADBEEFull;

        ctx.weak = iron_rc_downgrade(ctx.strong);
        atomic_store(&ctx.signal, 0);
        atomic_store(&ctx.b_result_kind, 0);

        pthread_t ta, tb;
        TEST_ASSERT_EQUAL_INT(0,
            pthread_create(&ta, NULL, race_thread_drop_strong, &ctx));
        TEST_ASSERT_EQUAL_INT(0,
            pthread_create(&tb, NULL, race_thread_attempt_upgrade, &ctx));

        /* Signal both — they race. */
        atomic_store_explicit(&ctx.signal, 1, memory_order_release);

        TEST_ASSERT_EQUAL_INT(0, pthread_join(ta, NULL));
        TEST_ASSERT_EQUAL_INT(0, pthread_join(tb, NULL));

        int kind = atomic_load(&ctx.b_result_kind);
        if (kind == 1) {
            null_count++;
        } else if (kind == 2) {
            success_count++;
        } else {
            TEST_FAIL_MESSAGE("B thread did not record a result");
        }

        /* Tear down weak — block freed when both reach 0. */
        iron_weak_rc_release(ctx.weak);
    }

    /* Every iteration produced exactly one observation. */
    TEST_ASSERT_EQUAL_INT(UPGRADE_RACE_ITERS, null_count + success_count);

    /* The race window MUST be exercised — at least one iteration in 10000
     * should produce NULL. If null_count == 0 the test is broken (always-
     * success means the upgrader is somehow ALWAYS racing ahead of the
     * dropper — symptom of a missing acquire-load or a wrong ordering).
     *
     * Mirror invariant: success_count >= 1 too (always-NULL would mean the
     * upgrader NEVER wins the race, which is statistically implausible
     * but not a correctness failure on its own). We assert null_count >=
     * 1 because that is the safety-critical direction (the upgrader MUST
     * be able to observe refcount==0 reliably). */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, null_count);
}

/* ── Unity entrypoint ─────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_upgrade_on_null_weak_returns_null);
    RUN_TEST(test_upgrade_while_strong_alive_succeeds);
    RUN_TEST(test_upgrade_after_strong_drop_returns_null);
    RUN_TEST(test_upgrade_mid_destructor_race_returns_null_or_success);
    return UNITY_END();
}
