/* test_lsp_mailbox_coalesce -- Phase 2 Plan 05 Task 01 (CORE-15).
 *
 * Exercises the coalescing invariant of src/lsp/workers/mailbox.c: rapid
 * COMPILE posts must collapse to a single pending slot holding the
 * newest version. SHUTDOWN is never coalesced with COMPILE; it has
 * strictly higher priority at dequeue.
 *
 * Tests use only the mailbox API (no worker thread). Multi-producer
 * concurrency stress runs briefly to shake ASan/TSan hazards. */

#include "unity.h"
#include "lsp/workers/mailbox.h"
#include "runtime/iron_runtime.h"   /* iron_thread_t, IRON_THREAD_* */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. Two rapid COMPILE posts collapse to the second (newest) ─────── */
static void test_compile_coalesce_two(void) {
    IronLsp_Mailbox *m = ilsp_mailbox_create();
    TEST_ASSERT_NOT_NULL(m);

    ilsp_mailbox_post_compile(m, 1, NULL);
    ilsp_mailbox_post_compile(m, 2, NULL);

    IronLsp_MailboxMsg msg = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_COMPILE, msg.kind);
    TEST_ASSERT_EQUAL_INT32(2, msg.version);

    /* Second dequeue would block -- confirm nothing else is queued by
     * posting SHUTDOWN and dequeueing that. */
    ilsp_mailbox_post_shutdown(m);
    IronLsp_MailboxMsg msg2 = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_SHUTDOWN, msg2.kind);

    ilsp_mailbox_destroy(m);
}

/* ── 2. 100 rapid COMPILE posts collapse to the last one ────────────── */
static void test_compile_coalesce_100(void) {
    IronLsp_Mailbox *m = ilsp_mailbox_create();
    for (int32_t v = 1; v <= 100; v++) {
        ilsp_mailbox_post_compile(m, v, NULL);
    }
    IronLsp_MailboxMsg msg = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_COMPILE, msg.kind);
    TEST_ASSERT_EQUAL_INT32(100, msg.version);

    ilsp_mailbox_post_shutdown(m);
    IronLsp_MailboxMsg msg2 = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_SHUTDOWN, msg2.kind);
    ilsp_mailbox_destroy(m);
}

/* ── 3. Post-dequeue + post again yields the second post ────────────── */
static void test_compile_dequeue_then_post_again(void) {
    IronLsp_Mailbox *m = ilsp_mailbox_create();
    ilsp_mailbox_post_compile(m, 1, NULL);
    IronLsp_MailboxMsg first = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT32(1, first.version);

    ilsp_mailbox_post_compile(m, 2, NULL);
    IronLsp_MailboxMsg second = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_COMPILE, second.kind);
    TEST_ASSERT_EQUAL_INT32(2, second.version);

    ilsp_mailbox_post_shutdown(m);
    (void)ilsp_mailbox_dequeue(m);
    ilsp_mailbox_destroy(m);
}

/* ── 4. SHUTDOWN + COMPILE: SHUTDOWN dequeues first (priority), COMPILE
 *    is NOT discarded -- subsequent dequeue returns it. ───────────── */
static void test_shutdown_priority_over_compile(void) {
    IronLsp_Mailbox *m = ilsp_mailbox_create();
    ilsp_mailbox_post_compile(m, 7, NULL);
    ilsp_mailbox_post_shutdown(m);

    IronLsp_MailboxMsg first = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_SHUTDOWN, first.kind);

    IronLsp_MailboxMsg second = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_COMPILE, second.kind);
    TEST_ASSERT_EQUAL_INT32(7, second.version);

    ilsp_mailbox_destroy(m);
}

/* ── 5. Multi-producer: two threads post COMPILEs concurrently; the
 *    mailbox must always hold a valid COMPILE slot -- never a torn
 *    version -- and a single dequeue observes one of the posted values.
 *    ASan + pthread race detector validates this. ────────────────── */

typedef struct {
    IronLsp_Mailbox *m;
    int32_t          base;
    int              count;
} producer_arg_t;

static void *producer_fn(void *arg) {
    producer_arg_t *p = (producer_arg_t *)arg;
    for (int i = 0; i < p->count; i++) {
        ilsp_mailbox_post_compile(p->m, p->base + (int32_t)i, NULL);
    }
    return NULL;
}

static void test_multi_producer_coalesce(void) {
    IronLsp_Mailbox *m = ilsp_mailbox_create();

    producer_arg_t a = { m,  1,    200 };  /* posts versions 1..200 */
    producer_arg_t b = { m, 1000, 200 };  /* posts versions 1000..1199 */

    iron_thread_t t1, t2;
    TEST_ASSERT_EQUAL_INT(0, IRON_THREAD_CREATE(t1, producer_fn, &a));
    TEST_ASSERT_EQUAL_INT(0, IRON_THREAD_CREATE(t2, producer_fn, &b));

    IRON_THREAD_JOIN(t1);
    IRON_THREAD_JOIN(t2);

    /* After both producers finish, at most one COMPILE should be in the
     * mailbox. Dequeue it and assert the version is one of the posted
     * values (not torn). */
    IronLsp_MailboxMsg msg = ilsp_mailbox_dequeue(m);
    TEST_ASSERT_EQUAL_INT(ILSP_MSG_COMPILE, msg.kind);
    bool in_a_range = (msg.version >= 1    && msg.version <= 200);
    bool in_b_range = (msg.version >= 1000 && msg.version <= 1199);
    TEST_ASSERT_TRUE_MESSAGE(in_a_range || in_b_range,
        "coalesced version must be one of the posted values -- torn write?");

    ilsp_mailbox_post_shutdown(m);
    (void)ilsp_mailbox_dequeue(m);
    ilsp_mailbox_destroy(m);
}

/* ── 6. shutdown_pending() reports the flag without dequeue ─────────── */
static void test_shutdown_pending_visibility(void) {
    IronLsp_Mailbox *m = ilsp_mailbox_create();
    TEST_ASSERT_FALSE(ilsp_mailbox_shutdown_pending(m));
    ilsp_mailbox_post_shutdown(m);
    TEST_ASSERT_TRUE(ilsp_mailbox_shutdown_pending(m));
    (void)ilsp_mailbox_dequeue(m);
    /* After consuming the shutdown slot the flag remains set (it's a
     * sticky marker) -- intentional: once the worker knows to shut
     * down, additional posts must not revive it. */
    TEST_ASSERT_TRUE(ilsp_mailbox_shutdown_pending(m));
    ilsp_mailbox_destroy(m);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_compile_coalesce_two);
    RUN_TEST(test_compile_coalesce_100);
    RUN_TEST(test_compile_dequeue_then_post_again);
    RUN_TEST(test_shutdown_priority_over_compile);
    RUN_TEST(test_multi_producer_coalesce);
    RUN_TEST(test_shutdown_pending_visibility);
    return UNITY_END();
}
