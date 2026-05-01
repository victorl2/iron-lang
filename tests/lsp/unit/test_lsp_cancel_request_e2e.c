/* test_lsp_cancel_request_e2e -- Phase 2 Plan 03 Task 03.
 *
 * Exercises the per-request cancel registry API and the end-to-end
 * $/cancelRequest dispatch path (CORE-14). The registry is a
 * string-keyed stb_ds map whose values are heap-owned _Atomic bool*
 * pointers. The Plan 01 HARD-05 cancel-poll primitive (memory_order_relaxed
 * atomic_load_explicit) is what consumers on worker threads use to
 * observe the flag without a lock. */
#include "unity.h"
#include "lsp/server/server.h"
#include "lsp/server/cancel.h"
#include "lsp/server/dispatch.h"
#include "lsp/server/lifecycle.h"
#include "lsp/server/dyn_register.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/json.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern IronLsp_DynRegister *ilsp_dyn_register_create(void);
extern void                 ilsp_dyn_register_destroy(IronLsp_DynRegister *r);

void setUp(void)    {}
void tearDown(void) {}

/* HARD-05 cancel-poll primitive copy (per "each TU defines its own" rule
 * in lexer.c:12-18). */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag != NULL && atomic_load_explicit(flag, memory_order_relaxed);
}

/* ── Test 1: register returns non-NULL atomic bool initialized false ── */
static void test_cancel_register_returns_false_atomic(void) {
    IronLsp_CancelRegistry *r = ilsp_cancel_registry_create();
    TEST_ASSERT_NOT_NULL(r);

    _Atomic bool *f = ilsp_cancel_register(r, "42");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FALSE(atomic_load(f));

    ilsp_cancel_unregister(r, "42");
    ilsp_cancel_registry_destroy(r);
}

/* ── Test 2: signal flips the flag ──────────────────────────────────── */
static void test_cancel_signal_flips_flag(void) {
    IronLsp_CancelRegistry *r = ilsp_cancel_registry_create();
    _Atomic bool *f = ilsp_cancel_register(r, "42");
    TEST_ASSERT_FALSE(iron_cancel_requested(f));

    bool ok = ilsp_cancel_signal(r, "42");
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(iron_cancel_requested(f));

    ilsp_cancel_unregister(r, "42");
    ilsp_cancel_registry_destroy(r);
}

/* ── Test 3: signal on unknown id returns false ────────────────────── */
static void test_cancel_signal_unknown_id_returns_false(void) {
    IronLsp_CancelRegistry *r = ilsp_cancel_registry_create();
    bool ok = ilsp_cancel_signal(r, "99");
    TEST_ASSERT_FALSE(ok);
    ilsp_cancel_registry_destroy(r);
}

/* ── Test 4: unregister makes subsequent signal a no-op ────────────── */
static void test_cancel_unregister_frees(void) {
    IronLsp_CancelRegistry *r = ilsp_cancel_registry_create();
    (void)ilsp_cancel_register(r, "42");
    ilsp_cancel_unregister(r, "42");

    bool ok = ilsp_cancel_signal(r, "42");
    TEST_ASSERT_FALSE(ok);

    ilsp_cancel_registry_destroy(r);
}

/* ── Test 5: $/cancelRequest dispatch path flips the flag ─────────── */
static void test_cancel_handler_end_to_end(void) {
    char *buf = NULL; size_t len = 0;
    FILE *sink = open_memstream(&buf, &len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    IronLsp_Server s; memset(&s, 0, sizeof(s));
    s.lifecycle = ILSP_LIFECYCLE_RUNNING;   /* cancel is allowed in RUNNING */
    s.writer    = w;
    s.cancels   = ilsp_cancel_registry_create();
    s.dyn_reg   = ilsp_dyn_register_create();
    atomic_store(&s.next_request_id, 1);

    /* Register id=7 and grab the flag. */
    _Atomic bool *f = ilsp_cancel_register(s.cancels, "7");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FALSE(iron_cancel_requested(f));

    /* Feed a $/cancelRequest for id=7 through the dispatcher. */
    Iron_Arena a = iron_arena_create(4 * 1024);
    const char *body =
        "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\","
        "\"params\":{\"id\":7}}";
    ilsp_dispatch_route(&s, body, strlen(body), &a);

    TEST_ASSERT_TRUE(iron_cancel_requested(f));

    iron_arena_free(&a);
    ilsp_cancel_unregister(s.cancels, "7");
    ilsp_cancel_registry_destroy(s.cancels);
    ilsp_dyn_register_destroy(s.dyn_reg);
    ilsp_writer_destroy(w);
    fclose(sink);
    free(buf);
}

/* ── Test 6: worker thread observes the flip within a tight budget ── */
typedef struct {
    _Atomic bool *flag;
    _Atomic bool  done;
    long          observed_ns;
} WorkerCtx;

static void *polling_worker(void *arg) {
    WorkerCtx *wc = (WorkerCtx *)arg;
    struct timespec start; clock_gettime(CLOCK_MONOTONIC, &start);

    /* Busy-poll up to 100ms. */
    const long budget_ns = 100L * 1000L * 1000L;
    for (;;) {
        if (iron_cancel_requested(wc->flag)) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            wc->observed_ns = (now.tv_sec  - start.tv_sec) * 1000000000L +
                              (now.tv_nsec - start.tv_nsec);
            atomic_store(&wc->done, true);
            return NULL;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec  - start.tv_sec) * 1000000000L +
                       (now.tv_nsec - start.tv_nsec);
        if (elapsed > budget_ns) {
            wc->observed_ns = -1;   /* timed out */
            atomic_store(&wc->done, true);
            return NULL;
        }
    }
}

static void test_cancel_thread_visibility(void) {
    IronLsp_CancelRegistry *r = ilsp_cancel_registry_create();
    _Atomic bool *f = ilsp_cancel_register(r, "99");

    WorkerCtx wc;
    wc.flag = f;
    atomic_store(&wc.done, false);
    wc.observed_ns = 0;
    pthread_t th;
    pthread_create(&th, NULL, polling_worker, &wc);

    /* Give the worker a hair of time to enter the loop, then signal. */
    struct timespec small = { .tv_sec = 0, .tv_nsec = 1000000L };  /* 1ms */
    nanosleep(&small, NULL);
    bool ok = ilsp_cancel_signal(r, "99");
    TEST_ASSERT_TRUE(ok);

    pthread_join(th, NULL);

    /* Visibility primitive is memory_order_relaxed on x86/arm64 with
     * implicit release-acquire for aligned-word stores; typical
     * observation is ~microseconds. The original 10ms budget held on
     * dev hardware but flaked on GitHub-hosted macos-latest runners
     * (worker scheduling under shared-host load can push observation
     * past 10ms). Widen to 100ms — still tight enough to catch a
     * missing-fence regression (would observe never / -1 / seconds)
     * without being flaky on noisy CI. */
    TEST_ASSERT_TRUE(wc.observed_ns >= 0);
    TEST_ASSERT_TRUE(wc.observed_ns < 100L * 1000L * 1000L);

    ilsp_cancel_unregister(r, "99");
    ilsp_cancel_registry_destroy(r);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cancel_register_returns_false_atomic);
    RUN_TEST(test_cancel_signal_flips_flag);
    RUN_TEST(test_cancel_signal_unknown_id_returns_false);
    RUN_TEST(test_cancel_unregister_frees);
    RUN_TEST(test_cancel_handler_end_to_end);
    RUN_TEST(test_cancel_thread_visibility);
    return UNITY_END();
}
