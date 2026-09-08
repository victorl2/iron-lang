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
#include <errno.h>
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

/* Thread visibility is a correctness property, not a scheduler latency
 * benchmark. Handshake before signaling, then allow bounded completion.
 * In particular, relaxed atomics do NOT imply release/acquire ordering. */
typedef struct {
    _Atomic bool *flag;
    _Atomic bool  ready;
    _Atomic bool  done;
    _Atomic bool  stop;
    long          startup_delay_ms;
    long          poll_delay_ms;
    bool          observed;
    int           error;
} WorkerCtx;

static int delay_ms(long ms) {
    struct timespec remaining = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return errno;
    }
    return 0;
}

/* Return 1 for completion, 0 for timeout, or a negative errno. The timeout
 * is only a hang guard; no assertion depends on sub-second scheduling. */
static int wait_for_flag(const _Atomic bool *flag, long timeout_ms) {
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return -errno;
    for (;;) {
        if (atomic_load_explicit(flag, memory_order_acquire)) return 1;
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -errno;
        long elapsed_ns = (now.tv_sec - start.tv_sec) * 1000000000L +
                          now.tv_nsec - start.tv_nsec;
        if (elapsed_ns >= timeout_ms * 1000000L) return 0;
        int error = delay_ms(1);
        if (error) return -error;
    }
}

static void *polling_worker(void *arg) {
    WorkerCtx *wc = (WorkerCtx *)arg;
    wc->error = delay_ms(wc->startup_delay_ms);
    if (wc->error) goto finished;
    atomic_store_explicit(&wc->ready, true, memory_order_release);
    wc->error = delay_ms(wc->poll_delay_ms);
    if (wc->error) goto finished;

    while (!atomic_load_explicit(&wc->stop, memory_order_relaxed)) {
        if (iron_cancel_requested(wc->flag)) {
            wc->observed = true;
            break;
        }
        wc->error = delay_ms(1);
        if (wc->error) break;
    }
finished:
    atomic_store_explicit(&wc->done, true, memory_order_release);
    return NULL;
}

static void check_thread_visibility(long startup_delay_ms, long signal_delay_ms,
                                    long poll_delay_ms, bool send_signal) {
    IronLsp_CancelRegistry *r = ilsp_cancel_registry_create();
    TEST_ASSERT_NOT_NULL(r);
    _Atomic bool *f = ilsp_cancel_register(r, "99");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FALSE(iron_cancel_requested(f));

    WorkerCtx wc = { .flag = f, .startup_delay_ms = startup_delay_ms,
                     .poll_delay_ms = poll_delay_ms };
    atomic_init(&wc.ready, false);
    atomic_init(&wc.done, false);
    atomic_init(&wc.stop, false);
    pthread_t th;
    int create_error = pthread_create(&th, NULL, polling_worker, &wc);
    if (create_error) {
        ilsp_cancel_registry_destroy(r);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, create_error, "pthread_create failed");
        return;
    }

    /* This synchronization precedes cancellation. There is deliberately no
     * producer-to-worker handshake after the store to the cancellation flag:
     * the worker must observe it through the actual relaxed polling primitive. */
    int ready = wait_for_flag(&wc.ready, 5000);
    int delay_error = 0;
    bool signaled = false;
    if (ready == 1 && send_signal) {
        delay_error = delay_ms(signal_delay_ms);
        if (!delay_error) signaled = ilsp_cancel_signal(r, "99");
    }
    int completed = wait_for_flag(&wc.done, send_signal ? 5000 : 50);

    /* Stop and join even on failure, before Unity can longjmp. Never free
     * the registry flag while the worker might still read it. CTest provides
     * a final process-level timeout for a stuck worker/join. */
    atomic_store_explicit(&wc.stop, true, memory_order_relaxed);
    int join_error = pthread_join(th, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, join_error, "pthread_join failed");
    ilsp_cancel_unregister(r, "99");
    ilsp_cancel_registry_destroy(r);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ready, "worker did not become ready");
    TEST_ASSERT_EQUAL_INT(0, delay_error);
    TEST_ASSERT_EQUAL_INT(0, wc.error);
    TEST_ASSERT_EQUAL_INT(send_signal, signaled);
    TEST_ASSERT_EQUAL_INT_MESSAGE(send_signal ? 1 : 0, completed,
        "worker completion must require cancellation, within the hang guard");
    TEST_ASSERT_EQUAL_INT(send_signal, wc.observed);
}

static void test_cancel_thread_visibility(void) {
    check_thread_visibility(0, 0, 0, true);
}

static void test_cancel_thread_visibility_delayed_start(void) {
    check_thread_visibility(200, 0, 0, true);
}

static void test_cancel_thread_visibility_delayed_signal(void) {
    check_thread_visibility(0, 200, 0, true);
}

static void test_cancel_thread_visibility_delayed_poll(void) {
    check_thread_visibility(0, 0, 200, true);
}

static void test_cancel_thread_visibility_requires_signal(void) {
    check_thread_visibility(0, 0, 0, false);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cancel_register_returns_false_atomic);
    RUN_TEST(test_cancel_signal_flips_flag);
    RUN_TEST(test_cancel_signal_unknown_id_returns_false);
    RUN_TEST(test_cancel_unregister_frees);
    RUN_TEST(test_cancel_handler_end_to_end);
    RUN_TEST(test_cancel_thread_visibility);
    RUN_TEST(test_cancel_thread_visibility_delayed_start);
    RUN_TEST(test_cancel_thread_visibility_delayed_signal);
    RUN_TEST(test_cancel_thread_visibility_delayed_poll);
    RUN_TEST(test_cancel_thread_visibility_requires_signal);
    return UNITY_END();
}
