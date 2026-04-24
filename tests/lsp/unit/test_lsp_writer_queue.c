/* test_lsp_writer_queue -- Phase 2 Plan 02 Task 03.
 *
 * Exercises the bounded writer queue + drop policy (T-02-04) and the
 * reader thread's framer pump. The tests use fmemopen / open_memstream
 * (glibc) for the writer sink and fmemopen for the reader source so that
 * assertions can inspect the exact bytes written without touching stdout.
 *
 * Drop policy covered:
 *   - All-responses full queue -> FULL_DROPPED
 *   - One log + 255 responses full -> next response OK_DROPPED_LOG
 *   - FIFO ordering within a priority class
 *   - Shutdown drains queued items then exits
 *   - Reader callback fires once per COMPLETE frame
 */
#include "unity.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/reader.h"
#include "lsp/transport/frame.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {}

/* Helper: malloc + memcpy a body string so it can be donated to the
 * writer (which takes ownership). */
static char *dup_body(const char *s, size_t *out_len) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n);
    memcpy(p, s, n);
    if (out_len) *out_len = n;
    return p;
}

/* ── Test 1: FIFO within priority ───────────────────────────────────────── */
static void test_fifo_within_priority(void) {
    /* No thread -- enqueue, then dequeue via ilsp_writer_drain_one for
     * deterministic inspection. */
    char *sink_buf = NULL;
    size_t sink_len = 0;
    FILE *sink = open_memstream(&sink_buf, &sink_len);
    TEST_ASSERT_NOT_NULL(sink);

    IronLsp_Writer *w = ilsp_writer_create(sink);
    TEST_ASSERT_NOT_NULL(w);

    /* NB: must sequence dup_body BEFORE reading the length — C argument
     * evaluation order is unspecified, so inlining `dup_body(..., &l1)` and
     * `l1` as sibling args would pass the pre-dup uninitialized value under
     * some compilers (gcc evaluates right-to-left). Rule 1 bugfix (08-02). */
    size_t l1 = 0, l2 = 0, l3 = 0;
    char *b1 = dup_body("R1", &l1);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, b1, l1));
    char *b2 = dup_body("R2", &l2);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, b2, l2));
    char *b3 = dup_body("R3", &l3);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, b3, l3));

    /* Drain all three serially on the calling thread. */
    TEST_ASSERT_TRUE(ilsp_writer_drain_one(w));
    TEST_ASSERT_TRUE(ilsp_writer_drain_one(w));
    TEST_ASSERT_TRUE(ilsp_writer_drain_one(w));
    TEST_ASSERT_FALSE(ilsp_writer_drain_one(w));  /* queue empty */

    fclose(sink);
    /* Buffer now holds three framed messages in order. */
    TEST_ASSERT_NOT_NULL(sink_buf);
    const char *expected =
        "Content-Length: 2\r\n\r\nR1"
        "Content-Length: 2\r\n\r\nR2"
        "Content-Length: 2\r\n\r\nR3";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), sink_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, sink_buf, sink_len);

    free(sink_buf);
    ilsp_writer_destroy(w);
}

/* ── Test 2: full queue of all-responses rejects new response ───────────── */
static void test_all_responses_full_drops_new(void) {
    char *sink_buf = NULL;
    size_t sink_len = 0;
    FILE *sink = open_memstream(&sink_buf, &sink_len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    /* Fill the queue with ILSP_WRITER_QUEUE_CAPACITY responses. */
    for (int i = 0; i < ILSP_WRITER_QUEUE_CAPACITY; i++) {
        size_t len;
        char *body = dup_body("R", &len);
        IronLsp_EnqueueResult rc =
            ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len);
        TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK, rc);
    }

    /* 257th enqueue: no lower-priority item to drop, so FULL_DROPPED. */
    size_t len;
    char *body = dup_body("OVERFLOW", &len);
    IronLsp_EnqueueResult rc =
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_FULL_DROPPED, rc);

    fclose(sink);
    free(sink_buf);
    ilsp_writer_destroy(w);
}

/* ── Test 3: drop log before response ───────────────────────────────────── */
static void test_drop_log_before_response(void) {
    char *sink_buf = NULL;
    size_t sink_len = 0;
    FILE *sink = open_memstream(&sink_buf, &sink_len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    /* Enqueue 1 log first, then fill with responses up to capacity-1. */
    {
        size_t len;
        char *body = dup_body("LOG", &len);
        TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
            ilsp_writer_enqueue(w, ILSP_PRIO_LOG, body, len));
    }
    for (int i = 0; i < ILSP_WRITER_QUEUE_CAPACITY - 1; i++) {
        size_t len;
        char *body = dup_body("R", &len);
        TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
            ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len));
    }

    /* Now queue full (1 log + 255 responses). Next response must drop
     * the log, not any response. */
    size_t len;
    char *body = dup_body("PROMOTED", &len);
    IronLsp_EnqueueResult rc =
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK_DROPPED_LOG, rc);

    /* Drain and confirm no "LOG" body in the sink (responses only). */
    while (ilsp_writer_drain_one(w)) { /* spin */ }
    fclose(sink);
    TEST_ASSERT_NOT_NULL(sink_buf);
    /* "LOG" must not appear in the framed output. */
    TEST_ASSERT_NULL(memmem(sink_buf, sink_len, "\r\n\r\nLOG", 7));
    /* "PROMOTED" should appear (it was enqueued after the drop). */
    TEST_ASSERT_NOT_NULL(memmem(sink_buf, sink_len,
                                "\r\n\r\nPROMOTED", 12));

    free(sink_buf);
    ilsp_writer_destroy(w);
}

/* ── Test 4: drop notification before response when no logs ─────────────── */
static void test_drop_notification_when_no_logs(void) {
    char *sink_buf = NULL;
    size_t sink_len = 0;
    FILE *sink = open_memstream(&sink_buf, &sink_len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    /* 1 notification, then fill with responses. */
    {
        size_t len;
        char *body = dup_body("NOTIF", &len);
        TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
            ilsp_writer_enqueue(w, ILSP_PRIO_NOTIFICATION, body, len));
    }
    for (int i = 0; i < ILSP_WRITER_QUEUE_CAPACITY - 1; i++) {
        size_t len;
        char *body = dup_body("R", &len);
        TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
            ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len));
    }

    size_t len;
    char *body = dup_body("PROMOTED", &len);
    IronLsp_EnqueueResult rc =
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK_DROPPED_NOTIFICATION, rc);

    while (ilsp_writer_drain_one(w)) { /* spin */ }
    fclose(sink);
    TEST_ASSERT_NOT_NULL(sink_buf);
    TEST_ASSERT_NULL(memmem(sink_buf, sink_len, "\r\n\r\nNOTIF", 9));

    free(sink_buf);
    ilsp_writer_destroy(w);
}

/* ── Test 5: writer thread -- start/enqueue/shutdown drains cleanly ────── */
static void test_writer_thread_drains_on_shutdown(void) {
    char *sink_buf = NULL;
    size_t sink_len = 0;
    FILE *sink = open_memstream(&sink_buf, &sink_len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    ilsp_writer_start(w);
    size_t len;
    char *body = dup_body("HELLO", &len);
    TEST_ASSERT_EQUAL_INT(ILSP_ENQUEUE_OK,
        ilsp_writer_enqueue(w, ILSP_PRIO_RESPONSE, body, len));

    ilsp_writer_shutdown(w);
    ilsp_writer_join(w);

    fclose(sink);
    TEST_ASSERT_NOT_NULL(sink_buf);
    const char *expected = "Content-Length: 5\r\n\r\nHELLO";
    TEST_ASSERT_EQUAL_size_t(strlen(expected), sink_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, sink_buf, sink_len);

    free(sink_buf);
    ilsp_writer_destroy(w);
}

/* ── Test 6: reader thread feeds framer -> on_message callback ─────────── */
typedef struct {
    int    call_count;
    char   last_body[32];
    size_t last_len;
} reader_ctx_t;

static void reader_on_message(void *ctx, const char *body, size_t len) {
    reader_ctx_t *rc = (reader_ctx_t *)ctx;
    rc->call_count++;
    size_t n = len < sizeof(rc->last_body) - 1 ? len : sizeof(rc->last_body) - 1;
    memcpy(rc->last_body, body, n);
    rc->last_body[n] = '\0';
    rc->last_len = len;
}

static void test_reader_feeds_framer(void) {
    const char *input = "Content-Length: 4\r\n\r\ntest";
    FILE *src = fmemopen((void *)input, strlen(input), "r");
    TEST_ASSERT_NOT_NULL(src);

    reader_ctx_t rc = {0};
    IronLsp_Reader *r = ilsp_reader_create(src, reader_on_message, &rc);
    TEST_ASSERT_NOT_NULL(r);

    ilsp_reader_start(r);
    ilsp_reader_join(r);  /* blocks until feof drains */

    TEST_ASSERT_EQUAL_INT(1, rc.call_count);
    TEST_ASSERT_EQUAL_size_t(4, rc.last_len);
    TEST_ASSERT_EQUAL_STRING("test", rc.last_body);

    ilsp_reader_destroy(r);
    fclose(src);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fifo_within_priority);
    RUN_TEST(test_all_responses_full_drops_new);
    RUN_TEST(test_drop_log_before_response);
    RUN_TEST(test_drop_notification_when_no_logs);
    RUN_TEST(test_writer_thread_drains_on_shutdown);
    RUN_TEST(test_reader_feeds_framer);
    return UNITY_END();
}
