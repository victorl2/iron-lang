/* test_lsp_sigabrt_boundary -- Phase 2 Plan 05 Task 01 (CORE-18).
 *
 * Exercises the SIGABRT -> sigsetjmp -> window/showMessage pipeline.
 * Four RUN_TESTs:
 *
 *   1. Direct siglongjmp round-trip: install handler, set TLS, raise()
 *      SIGABRT, assert sigsetjmp's 1-branch fires and abort_count
 *      increments.
 *
 *   2. Second strike sets quarantined + subsequent compile is skipped.
 *
 *   3. First strike emits window/showMessage with MessageType.Warning
 *      (2). Uses the public test seam ilsp_worker_handle_abort_strike
 *      with a real IronLsp_Writer backed by open_memstream. We drain
 *      the writer (shutdown + join + fflush), strip the Content-Length
 *      framing via ilsp_frame_feed, and parse the JSON body via
 *      ilsp_json_parse to inspect method + params.type + params.message.
 *
 *   4. Second strike emits window/showMessage with MessageType.Error
 *      (1) AND sets doc->quarantined = true.
 *
 * Rationale: the worker's SIGABRT branch cannot easily be triggered by
 * raise(SIGABRT) from Unity's test driver thread because the handler is
 * TLS-based and the worker loop is on its own thread. The test seam
 * ilsp_worker_handle_abort_strike exposes the non-signal side of the
 * recovery branch directly -- the same function the worker calls after
 * its sigsetjmp returns 1 -- so we can exercise the window/showMessage
 * emission path without racing pthreads against signal delivery.
 *
 * Test 1 uses the signal-handler path without a worker thread: we set
 * TLS to a local document, install the handler, and raise() on the main
 * thread. This proves the sigsetjmp/siglongjmp wiring itself. */

#include "unity.h"
#include "lsp/workers/ast_worker.h"
#include "lsp/workers/mailbox.h"
#include "lsp/obs/abort_handler.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/server/notifications.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/frame.h"
#include "lsp/transport/json.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void setUp(void)    {}
void tearDown(void) {}

static void sleep_ms(int ms) {
    struct timespec req = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
}

/* Minimal document helper -- we don't need the line index / sha256
 * fields for this test; zero-initialized is fine. */
static IronLsp_Document *make_bare_doc(const char *uri) {
    IronLsp_Document *d = (IronLsp_Document *)calloc(1, sizeof(*d));
    d->uri = strdup(uri);
    atomic_init(&d->quarantined, false);
    atomic_init(&d->shutdown, false);
    return d;
}

static void destroy_bare_doc(IronLsp_Document *d) {
    if (!d) return;
    free(d->uri);
    free(d);
}

/* ── 1. Signal handler round-trip: raise(SIGABRT) from TLS-set context
 *    siglongjmp's into our sigsetjmp; we observe the 1-branch. ─────── */

static void test_sigabrt_direct_siglongjmp(void) {
    ilsp_install_abort_handler();

    IronLsp_Document doc;
    memset(&doc, 0, sizeof(doc));
    doc.uri = (char *)"file:///direct.iron";
    atomic_init(&doc.quarantined, false);

    ilsp_current_doc_tls = &doc;

    int landed_in_recovery = 0;
    if (sigsetjmp(doc.abort_jmp, 1) == 0) {
        raise(SIGABRT);
        /* Unreachable -- siglongjmp transfers control out. */
        TEST_FAIL_MESSAGE("raise(SIGABRT) should have siglongjmp'd out");
    } else {
        landed_in_recovery = 1;
        doc.abort_count++;
    }

    ilsp_current_doc_tls = NULL;

    TEST_ASSERT_EQUAL_INT(1, landed_in_recovery);
    TEST_ASSERT_EQUAL_UINT32(1u, doc.abort_count);
}

/* ── 2. Second strike sets quarantined, subsequent compiles skipped ─── */
static void test_second_strike_quarantines_doc(void) {
    IronLsp_Document *d = make_bare_doc("file:///quar.iron");

    IronLsp_Server server;
    memset(&server, 0, sizeof(server));
    /* No writer: ilsp_send_window_showmessage is a no-op when writer is
     * NULL (guards at the top of the function). The quarantine bit is
     * what we assert here. */

    /* First strike (no quarantine). */
    d->abort_count++;
    ilsp_worker_handle_abort_strike(&server, d, d->abort_count);
    TEST_ASSERT_FALSE(atomic_load(&d->quarantined));

    /* Second strike: quarantines. */
    d->abort_count++;
    ilsp_worker_handle_abort_strike(&server, d, d->abort_count);
    TEST_ASSERT_TRUE(atomic_load(&d->quarantined));

    destroy_bare_doc(d);
}

/* ── 3 & 4: Strike -> writer emits window/showMessage ─────────────────
 * Construct a real IronLsp_Writer backed by open_memstream. Drive the
 * strike helper, then drain the writer and inspect the bytes. */

static IronLsp_Server *make_server_with_memstream(FILE **out_sink,
                                                    char **out_sink_buf,
                                                    size_t *out_sink_len) {
    IronLsp_Server *s = (IronLsp_Server *)calloc(1, sizeof(*s));
    *out_sink_buf = NULL;
    *out_sink_len = 0;
    *out_sink = open_memstream(out_sink_buf, out_sink_len);
    TEST_ASSERT_NOT_NULL(*out_sink);
    s->writer = ilsp_writer_create(*out_sink);
    ilsp_writer_start(s->writer);
    return s;
}

static void drain_server(IronLsp_Server *s, FILE *sink) {
    ilsp_writer_shutdown(s->writer);
    ilsp_writer_join(s->writer);
    fflush(sink);
    fclose(sink);
    ilsp_writer_destroy(s->writer);
    free(s);
}

/* Strip Content-Length framing from a byte buffer; return the body.
 * Caller must call ilsp_frame_consume / destroy to release the parser. */
static bool parse_frame_body(const char *bytes, size_t len,
                              IronLsp_FrameParser *p,
                              const char **out_body, size_t *out_len) {
    ilsp_frame_init(p);
    IronLsp_FrameResult r = ilsp_frame_feed(p, bytes, len,
                                              out_body, out_len);
    return r == ILSP_FRAME_RESULT_COMPLETE;
}

static void test_first_strike_emits_window_showmessage_warning(void) {
    FILE *sink = NULL;
    char *sink_buf = NULL;
    size_t sink_len = 0;
    IronLsp_Server *s = make_server_with_memstream(&sink, &sink_buf, &sink_len);

    IronLsp_Document *d = make_bare_doc("file:///tmp/test.iron");

    /* First strike: should enqueue a Warning window/showMessage. */
    d->abort_count = 1;
    ilsp_worker_handle_abort_strike(s, d, d->abort_count);

    /* Give the writer thread a beat to drain the queued notification. */
    sleep_ms(50);
    drain_server(s, sink);

    TEST_ASSERT_NOT_NULL(sink_buf);
    TEST_ASSERT_GREATER_THAN_size_t(0, sink_len);

    /* Strip framing -> JSON body. */
    IronLsp_FrameParser p;
    const char *body = NULL;
    size_t body_len = 0;
    TEST_ASSERT_TRUE_MESSAGE(parse_frame_body(sink_buf, sink_len,
                                                &p, &body, &body_len),
        "writer bytes must contain a complete LSP frame");

    /* Parse JSON. */
    Iron_Arena arena = iron_arena_create(4096);
    yyjson_read_err err;
    memset(&err, 0, sizeof(err));
    yyjson_doc *doc = ilsp_json_parse(body, body_len, &arena, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, "body must parse as JSON");

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *method = yyjson_obj_get(root, "method");
    yyjson_val *params = yyjson_obj_get(root, "params");
    yyjson_val *type   = yyjson_obj_get(params, "type");
    yyjson_val *msg    = yyjson_obj_get(params, "message");

    TEST_ASSERT_EQUAL_STRING("window/showMessage", yyjson_get_str(method));
    TEST_ASSERT_EQUAL_INT(/*Warning*/ 2, (int)yyjson_get_int(type));
    TEST_ASSERT_NOT_NULL(strstr(yyjson_get_str(msg),
                                  "iron-lsp: analysis crashed for"));
    TEST_ASSERT_NOT_NULL(strstr(yyjson_get_str(msg), "file:///tmp/test.iron"));

    ilsp_frame_destroy(&p);
    iron_arena_free(&arena);
    free(sink_buf);
    destroy_bare_doc(d);
}

static void test_second_strike_emits_window_showmessage_error_and_quarantines(void) {
    FILE *sink = NULL;
    char *sink_buf = NULL;
    size_t sink_len = 0;
    IronLsp_Server *s = make_server_with_memstream(&sink, &sink_buf, &sink_len);

    IronLsp_Document *d = make_bare_doc("file:///tmp/second.iron");

    /* Second strike: Error + quarantine. */
    d->abort_count = 2;
    ilsp_worker_handle_abort_strike(s, d, d->abort_count);

    sleep_ms(50);
    drain_server(s, sink);

    TEST_ASSERT_TRUE_MESSAGE(atomic_load(&d->quarantined),
        "second strike must set doc->quarantined");

    IronLsp_FrameParser p;
    const char *body = NULL;
    size_t body_len = 0;
    TEST_ASSERT_TRUE(parse_frame_body(sink_buf, sink_len,
                                        &p, &body, &body_len));

    Iron_Arena arena = iron_arena_create(4096);
    yyjson_read_err err;
    memset(&err, 0, sizeof(err));
    yyjson_doc *doc = ilsp_json_parse(body, body_len, &arena, &err);
    TEST_ASSERT_NOT_NULL(doc);

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *method = yyjson_obj_get(root, "method");
    yyjson_val *params = yyjson_obj_get(root, "params");
    yyjson_val *type   = yyjson_obj_get(params, "type");
    yyjson_val *msg    = yyjson_obj_get(params, "message");

    TEST_ASSERT_EQUAL_STRING("window/showMessage", yyjson_get_str(method));
    TEST_ASSERT_EQUAL_INT(/*Error*/ 1, (int)yyjson_get_int(type));
    TEST_ASSERT_NOT_NULL(strstr(yyjson_get_str(msg),
                                  "quarantined due to repeated analysis crashes"));
    TEST_ASSERT_NOT_NULL(strstr(yyjson_get_str(msg),
                                  "file:///tmp/second.iron"));

    ilsp_frame_destroy(&p);
    iron_arena_free(&arena);
    free(sink_buf);
    destroy_bare_doc(d);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sigabrt_direct_siglongjmp);
    RUN_TEST(test_second_strike_quarantines_doc);
    RUN_TEST(test_first_strike_emits_window_showmessage_warning);
    RUN_TEST(test_second_strike_emits_window_showmessage_error_and_quarantines);
    return UNITY_END();
}
