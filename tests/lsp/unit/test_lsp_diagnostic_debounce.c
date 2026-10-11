/* test_lsp_diagnostic_debounce -- Phase 2 Plan 05 Task 01 (CORE-16).
 *
 * Drives a full ASTWorker thread against the coalescing mailbox and
 * asserts the 150ms debounce timing invariant:
 *
 *   1. Rapid COMPILE posts (< 50ms apart) trigger exactly one call into
 *      the facade (coalescing + debounce).
 *   2. A COMPILE posted after the previous debounced call completes
 *      triggers a second call.
 *
 * The test provides its own definitions of ilsp_facade_compile and
 * ilsp_facade_pull_diagnostic as link-time stubs (CMake is configured
 * to NOT compile src/lsp/facade/compile.c into this test executable).
 * Each stub simply increments a counter so the test can assert call
 * frequency. */

#include "unity.h"
#include "lsp/workers/ast_worker.h"
#include "lsp/workers/mailbox.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/facade/compile.h"
#include "runtime/iron_runtime.h"       /* IRON_MUTEX_*, timing primitives */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "support/posix_test.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── Stubs for the facade symbols referenced by ast_worker.c ──────────
 * The test executable is built WITHOUT src/lsp/facade/compile.c, so
 * these definitions satisfy the linker AND let the test count calls. */

static _Atomic int g_facade_compile_calls      = 0;
static _Atomic int g_facade_pull_calls         = 0;
static _Atomic int32_t g_last_compile_version  = 0;

/* Test 4: the compile holds here until the main thread has edited the
 * live document, then records what text and version it saw. */
static _Atomic bool g_race_mode      = false;
static _Atomic bool g_in_compile     = false;
static _Atomic bool g_edit_done      = false;
static _Atomic bool g_saw_old_text   = false;
static _Atomic int32_t g_saw_version = 0;
static IronLsp_Document *g_live_doc;

static void sleep_ms(int ms);

void ilsp_facade_compile(struct IronLsp_Server *server,
                          struct IronLsp_Document *doc,
                          const IronLsp_CompileRequest *req) {
    (void)server;
    if (atomic_load(&g_race_mode)) {
        atomic_store(&g_in_compile, true);
        for (int i = 0; i < 400 && !atomic_load(&g_edit_done); i++) sleep_ms(5);
        atomic_store(&g_saw_old_text, doc != g_live_doc && doc->text &&
                                      strcmp(doc->text, "func main() {}\n") == 0);
        atomic_store(&g_saw_version, doc->version);
    }
    atomic_fetch_add(&g_facade_compile_calls, 1);
    if (req) atomic_store(&g_last_compile_version, req->version);
}

void ilsp_facade_pull_diagnostic(struct IronLsp_Server *server,
                                  struct IronLsp_Document *doc,
                                  const char *request_id) {
    (void)server; (void)doc; (void)request_id;
    atomic_fetch_add(&g_facade_pull_calls, 1);
}

void ilsp_facade_pull_diagnostic_empty(struct IronLsp_Server *server,
                                        const char *request_id) {
    (void)server; (void)request_id;
    atomic_fetch_add(&g_facade_pull_calls, 1);
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(int ms) {
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
}

static bool wait_for_compile_calls(int expected, int timeout_ms) {
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (atomic_load(&g_facade_compile_calls) < expected) {
        if (now_ms() >= deadline) return false;
        sleep_ms(5);
    }
    return true;
}

/* Minimal fake document + server. The worker touches doc->mailbox,
 * doc->abort_jmp, doc->abort_count, doc->quarantined, doc->shutdown,
 * doc->uri. Nothing else needs to be wired. */
static IronLsp_Document *make_doc(const char *uri) {
    IronLsp_Document *d = (IronLsp_Document *)calloc(1, sizeof(*d));
    d->uri = strdup(uri);
    atomic_init(&d->quarantined, false);
    atomic_init(&d->shutdown, false);
    IRON_MUTEX_INIT(d->text_lock);   /* the worker snapshots the text */
    d->owns_lock = true;
    d->mailbox = ilsp_mailbox_create();
    return d;
}

static void destroy_doc(IronLsp_Document *d) {
    if (!d) return;
    if (d->mailbox) ilsp_mailbox_destroy(d->mailbox);
    IRON_MUTEX_DESTROY(d->text_lock);
    free(d->uri);
    free(d);
}

/* ── Test 1: 5 rapid COMPILEs -> exactly 1 facade call ──────────────── */
static void test_rapid_compiles_coalesce_to_one_call(void) {
    atomic_store(&g_facade_compile_calls, 0);
    atomic_store(&g_last_compile_version, 0);

    IronLsp_Server server;
    memset(&server, 0, sizeof(server));

    IronLsp_Document *doc = make_doc("file:///tmp/test.iron");
    TEST_ASSERT_TRUE(ilsp_ast_worker_start(&server, doc));

    /* Post 5 COMPILEs in rapid succession (all < 50ms apart, well within
     * the 150ms debounce window). */
    for (int32_t v = 1; v <= 5; v++) {
        ilsp_mailbox_post_compile(doc->mailbox, v, NULL);
        sleep_ms(10);  /* 10ms gap between posts */
    }

    /* Wait for the worker instead of assuming the CI host schedules it
     * within a fixed wall-clock interval. Once the call arrives, a full
     * debounce window without another call proves that the burst stayed
     * coalesced. */
    TEST_ASSERT_TRUE_MESSAGE(wait_for_compile_calls(1, 2000),
        "rapid-burst COMPILE did not run before the test deadline");
    sleep_ms(300);

    int calls = atomic_load(&g_facade_compile_calls);
    int32_t v = atomic_load(&g_last_compile_version);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, calls,
        "rapid-burst COMPILEs must coalesce to exactly one facade call");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(5, v,
        "coalesced call must see the newest version");

    ilsp_ast_worker_shutdown_and_join(doc);
    destroy_doc(doc);
}

/* ── Test 2: completed COMPILE, then COMPILE -> 2 facade calls ─────── */
static void test_spaced_compiles_trigger_two_calls(void) {
    atomic_store(&g_facade_compile_calls, 0);

    IronLsp_Server server;
    memset(&server, 0, sizeof(server));

    IronLsp_Document *doc = make_doc("file:///tmp/test2.iron");
    TEST_ASSERT_TRUE(ilsp_ast_worker_start(&server, doc));

    ilsp_mailbox_post_compile(doc->mailbox, 1, NULL);
    TEST_ASSERT_TRUE_MESSAGE(wait_for_compile_calls(1, 2000),
        "first spaced COMPILE did not run before the test deadline");

    ilsp_mailbox_post_compile(doc->mailbox, 2, NULL);
    TEST_ASSERT_TRUE_MESSAGE(wait_for_compile_calls(2, 2000),
        "second spaced COMPILE did not run before the test deadline");

    int calls = atomic_load(&g_facade_compile_calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, calls,
        "two well-spaced COMPILEs must trigger two facade calls");

    ilsp_ast_worker_shutdown_and_join(doc);
    destroy_doc(doc);
}

/* ── Test 3: debounce timing is roughly 150ms (observable jitter tol.)
 * Post one compile; measure how long it takes to see the facade call. */
static void test_debounce_observes_150ms(void) {
    atomic_store(&g_facade_compile_calls, 0);

    IronLsp_Server server;
    memset(&server, 0, sizeof(server));

    IronLsp_Document *doc = make_doc("file:///tmp/timing.iron");
    TEST_ASSERT_TRUE(ilsp_ast_worker_start(&server, doc));

    uint64_t t0 = now_ms();
    ilsp_mailbox_post_compile(doc->mailbox, 1, NULL);

    /* Spin-poll for the call; bail at 2s to prevent CI hangs. */
    uint64_t elapsed = 0;
    while (atomic_load(&g_facade_compile_calls) == 0) {
        sleep_ms(5);
        elapsed = now_ms() - t0;
        if (elapsed > 2000) break;
    }
    uint64_t observed = now_ms() - t0;

    /* Scheduling can delay a worker on a shared CI host, but it cannot
     * legitimately make the 150ms debounce expire early. Keep a strict
     * lower bound to catch a missing debounce and a generous upper bound
     * that matches the explicit test deadline. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(120, (int)observed,
        "observed debounce expired too early");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(2000, (int)observed,
        "observed debounce exceeded the test deadline");
    (void)elapsed;

    ilsp_ast_worker_shutdown_and_join(doc);
    destroy_doc(doc);
}

/* ── Test 4: the analysis runs on a copy of the text ──────────────────
 * The worker used to hand the live document to the facade, which read
 * text, line index and version while the main thread applied the next
 * edits (a data race; the diagnostics came out labelled with the newer
 * version, their ranges computed on the older text). */
static void test_compile_sees_a_snapshot(void) {
    atomic_store(&g_facade_compile_calls, 0);
    atomic_store(&g_race_mode, true);

    IronLsp_Server server;
    memset(&server, 0, sizeof(server));

    const char *v1 = "func main() {}\n";
    IronLsp_Document *doc = ilsp_document_create("file:///tmp/race.iron", v1,
                                                 strlen(v1), 1);
    TEST_ASSERT_NOT_NULL(doc);
    doc->mailbox = ilsp_mailbox_create();
    g_live_doc = doc;
    TEST_ASSERT_TRUE(ilsp_ast_worker_start(&server, doc));
    ilsp_mailbox_post_compile(doc->mailbox, 1, NULL);

    for (int i = 0; i < 400 && !atomic_load(&g_in_compile); i++) sleep_ms(5);
    TEST_ASSERT_TRUE_MESSAGE(atomic_load(&g_in_compile), "the compile did not start");
    const char *v2 = "func main() { val changed = 1 }\n";
    ilsp_document_lock(doc);   /* as the didChange handler does */
    bool ok = ilsp_document_apply_full_replace(doc, v2, strlen(v2), 2);
    ilsp_document_unlock(doc);
    TEST_ASSERT_TRUE(ok);
    atomic_store(&g_edit_done, true);
    TEST_ASSERT_TRUE(wait_for_compile_calls(1, 3000));

    TEST_ASSERT_TRUE_MESSAGE(atomic_load(&g_saw_old_text),
        "the analysis must see the text it was started on, not the live buffer");
    TEST_ASSERT_EQUAL_INT32(1, atomic_load(&g_saw_version));

    atomic_store(&g_race_mode, false);
    ilsp_ast_worker_shutdown_and_join(doc);
    ilsp_mailbox_destroy(doc->mailbox);
    doc->mailbox = NULL;
    ilsp_document_destroy(doc);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rapid_compiles_coalesce_to_one_call);
    RUN_TEST(test_spaced_compiles_trigger_two_calls);
    RUN_TEST(test_debounce_observes_150ms);
    RUN_TEST(test_compile_sees_a_snapshot);
    return UNITY_END();
}
