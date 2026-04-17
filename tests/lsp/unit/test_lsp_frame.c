/* test_lsp_frame -- Phase 2 Plan 02 Task 01.
 * Content-Length framing state machine. Covers single-chunk, byte-at-a-time,
 * LF-only lenience, empty body, oversize guard (T-02-02), malformed headers,
 * and two concatenated messages in one chunk (consume + re-feed). */
#include "unity.h"
#include "lsp/transport/frame.h"

#include <string.h>

static IronLsp_FrameParser g_parser;

void setUp(void)    { ilsp_frame_init(&g_parser); }
void tearDown(void) { ilsp_frame_destroy(&g_parser); }

/* ── Test 1: single-feed complete ───────────────────────────────────────── */
static void test_single_feed_complete(void) {
    const char *msg = "Content-Length: 2\r\n\r\n{}";
    const char *body = NULL;
    size_t len = 0;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_EQUAL_size_t(2, len);
    TEST_ASSERT_EQUAL_MEMORY("{}", body, 2);
}

/* ── Test 2: byte-at-a-time feed ────────────────────────────────────────── */
static void test_byte_at_a_time(void) {
    const char *msg = "Content-Length: 2\r\n\r\n{}";
    size_t n = strlen(msg);
    const char *body = NULL;
    size_t len = 0;
    int completes = 0;
    for (size_t i = 0; i < n; i++) {
        IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, &msg[i], 1,
                                                &body, &len);
        if (r == ILSP_FRAME_RESULT_COMPLETE) completes++;
        else TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_NEED_MORE, r);
    }
    TEST_ASSERT_EQUAL_INT(1, completes);
    TEST_ASSERT_EQUAL_size_t(2, len);
    TEST_ASSERT_EQUAL_MEMORY("{}", body, 2);
}

/* ── Test 3: LF-only lenience ───────────────────────────────────────────── */
static void test_lf_only_lenience(void) {
    const char *msg = "Content-Length: 2\n\n{}";
    const char *body = NULL;
    size_t len = 0;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_EQUAL_size_t(2, len);
    TEST_ASSERT_EQUAL_MEMORY("{}", body, 2);
}

/* ── Test 4: empty body ─────────────────────────────────────────────────── */
static void test_empty_body(void) {
    const char *msg = "Content-Length: 0\r\n\r\n";
    const char *body = NULL;
    size_t len = 123;  /* sentinel -- must be overwritten */
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_EQUAL_size_t(0, len);
    TEST_ASSERT_NOT_NULL(body);  /* must be non-NULL even for empty body */
}

/* ── Test 5: oversize rejection (T-02-02 mitigation) ────────────────────── */
static void test_oversize_rejected(void) {
    char hdr[128];
    /* 20MB is > 10MB limit */
    snprintf(hdr, sizeof(hdr), "Content-Length: %d\r\n\r\n", 20 * 1024 * 1024);
    const char *body = NULL;
    size_t len = 0;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, hdr, strlen(hdr),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_ERROR_OVERSIZED, r);
}

/* ── Test 6: malformed headers (no Content-Length) ──────────────────────── */
static void test_malformed_no_content_length(void) {
    const char *msg = "Content-Type: utf-8\r\n\r\n{}";
    const char *body = NULL;
    size_t len = 0;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_ERROR_MALFORMED, r);
}

/* ── Test 7: two concatenated messages ──────────────────────────────────── */
static void test_two_concatenated_messages(void) {
    const char *msg = "Content-Length: 2\r\n\r\n{}"
                      "Content-Length: 3\r\n\r\n[1]";
    size_t n = strlen(msg);
    const char *body = NULL;
    size_t len = 0;

    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, n, &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_EQUAL_size_t(2, len);
    TEST_ASSERT_EQUAL_MEMORY("{}", body, 2);

    /* Consume first; next feed of 0 bytes must yield second message. */
    ilsp_frame_consume(&g_parser);
    r = ilsp_frame_feed(&g_parser, NULL, 0, &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_EQUAL_size_t(3, len);
    TEST_ASSERT_EQUAL_MEMORY("[1]", body, 3);
    ilsp_frame_consume(&g_parser);
}

/* ── Test 8: case-insensitive Content-Length header match ───────────────── */
static void test_case_insensitive_header(void) {
    const char *msg = "content-length: 2\r\n\r\n{}";
    const char *body = NULL;
    size_t len = 0;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_EQUAL_size_t(2, len);
    TEST_ASSERT_EQUAL_MEMORY("{}", body, 2);
}

/* ── Test 9: Content-Length plus other headers (ignored) ────────────────── */
static void test_content_type_is_ignored(void) {
    const char *msg = "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n"
                      "Content-Length: 4\r\n\r\nhiya";
    const char *body = NULL;
    size_t len = 0;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_COMPLETE, r);
    TEST_ASSERT_EQUAL_size_t(4, len);
    TEST_ASSERT_EQUAL_MEMORY("hiya", body, 4);
}

/* ── Test 10: invariant -- out_body untouched on NEED_MORE ──────────────── */
static void test_out_body_untouched_on_need_more(void) {
    const char *msg = "Content-Length: 2\r\n\r\n";  /* no body yet */
    const char *body = (const char *)0xDEADBEEF;    /* sentinel */
    size_t len = 0xCAFEBABE;
    IronLsp_FrameResult r = ilsp_frame_feed(&g_parser, msg, strlen(msg),
                                            &body, &len);
    TEST_ASSERT_EQUAL_INT(ILSP_FRAME_RESULT_NEED_MORE, r);
    TEST_ASSERT_EQUAL_PTR((const char *)0xDEADBEEF, body);
    TEST_ASSERT_EQUAL_size_t(0xCAFEBABE, len);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_feed_complete);
    RUN_TEST(test_byte_at_a_time);
    RUN_TEST(test_lf_only_lenience);
    RUN_TEST(test_empty_body);
    RUN_TEST(test_oversize_rejected);
    RUN_TEST(test_malformed_no_content_length);
    RUN_TEST(test_two_concatenated_messages);
    RUN_TEST(test_case_insensitive_header);
    RUN_TEST(test_content_type_is_ignored);
    RUN_TEST(test_out_body_untouched_on_need_more);
    return UNITY_END();
}
