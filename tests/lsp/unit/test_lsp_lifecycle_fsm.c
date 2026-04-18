/* test_lsp_lifecycle_fsm -- Phase 2 Plan 03 Task 01.
 *
 * Exercises the LSP lifecycle finite-state machine and the dispatcher's
 * state-gate integration. Scenarios covered:
 *   1. UNINIT rejects non-lifecycle request with -32002
 *      ServerNotInitialized
 *   2. initialize transitions UNINIT -> INITIALIZING and enqueues a
 *      response with "capabilities"
 *   3. initialized notification transitions INITIALIZING -> RUNNING with
 *      no response
 *   4. Duplicate initialize (RUNNING) is rejected with -32600
 *      InvalidRequest
 *   5. After shutdown, a subsequent non-exit request is rejected with
 *      -32600 InvalidRequest
 *   6. exit uses ilsp_exit_fn; test captures exit code instead of
 *      terminating
 *
 * The tests use the writer's synchronous drain path (ilsp_writer_drain_one
 * against an open_memstream sink) so assertions can inspect exact bytes
 * without depending on a writer-thread schedule. */
#include "unity.h"
#include "lsp/server/server.h"
#include "lsp/server/dispatch.h"
#include "lsp/server/lifecycle.h"
#include "lsp/server/cancel.h"
#include "lsp/server/dyn_register.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/json.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* dyn_register internal constructors (declared in dyn_register.c). */
extern IronLsp_DynRegister *ilsp_dyn_register_create(void);
extern void                 ilsp_dyn_register_destroy(IronLsp_DynRegister *r);

/* Exit hook override: the production default terminates the process;
 * the test swaps in a capture so Unity can assert + continue. */
extern void (*ilsp_exit_fn)(int);
static int  g_captured_exit_code = -1;
static bool g_exit_called         = false;
static void capture_exit(int code) {
    g_captured_exit_code = code;
    g_exit_called        = true;
}

void setUp(void) {
    g_captured_exit_code = -1;
    g_exit_called        = false;
}
void tearDown(void) {}

/* ── Test harness: build a server + sink-backed writer + registry ─────── */
typedef struct {
    IronLsp_Server  server;
    IronLsp_Writer *writer;
    FILE           *sink;
    char           *sink_buf;
    size_t          sink_len;
    Iron_Arena      arena;
} Harness;

static void harness_init(Harness *h) {
    memset(h, 0, sizeof(*h));
    h->sink_buf = NULL;
    h->sink_len = 0;
    h->sink = open_memstream(&h->sink_buf, &h->sink_len);
    TEST_ASSERT_NOT_NULL(h->sink);
    h->writer = ilsp_writer_create(h->sink);
    TEST_ASSERT_NOT_NULL(h->writer);
    h->arena = iron_arena_create(16 * 1024);

    h->server.lifecycle         = ILSP_LIFECYCLE_UNINIT;
    h->server.writer            = h->writer;
    h->server.reader            = NULL;
    h->server.cancels           = ilsp_cancel_registry_create();
    h->server.dyn_reg           = ilsp_dyn_register_create();
    h->server.position_encoding = ILSP_ENC_UTF16;
    atomic_store(&h->server.next_request_id, 1);
}

static void harness_destroy(Harness *h) {
    /* Drain any queued output before closing the sink. */
    while (ilsp_writer_drain_one(h->writer)) { /* spin */ }
    ilsp_writer_destroy(h->writer);
    fclose(h->sink);
    free(h->sink_buf);
    ilsp_cancel_registry_destroy(h->server.cancels);
    ilsp_dyn_register_destroy(h->server.dyn_reg);
    iron_arena_free(&h->arena);
}

/* Drain everything the writer produced and return the flushed bytes. */
static void harness_flush(Harness *h) {
    while (ilsp_writer_drain_one(h->writer)) { /* spin */ }
    fflush(h->sink);
}

/* Route a message through the dispatcher. */
static void dispatch(Harness *h, const char *body) {
    ilsp_dispatch_route(&h->server, body, strlen(body), &h->arena);
}

/* Find the FIRST JSON body in the sink (skipping the Content-Length
 * framing). Returns a pointer into sink_buf at the first byte past the
 * "\r\n\r\n". */
static const char *first_body(const Harness *h) {
    if (!h->sink_buf || h->sink_len == 0) return NULL;
    const char *sep = strstr(h->sink_buf, "\r\n\r\n");
    return sep ? sep + 4 : NULL;
}

/* Parse the first response body with a fresh arena. */
static yyjson_doc *parse_first(Harness *h, Iron_Arena *parse_arena) {
    const char *body = first_body(h);
    if (!body) return NULL;
    size_t total_tail = h->sink_len - (size_t)(body - h->sink_buf);
    /* The tail may concatenate multiple messages; find the length of
     * just the first JSON object by scanning balanced braces. */
    size_t depth = 0;
    size_t len = 0;
    bool in_str = false;
    bool esc    = false;
    for (size_t i = 0; i < total_tail; i++) {
        char c = body[i];
        if (in_str) {
            if (esc)        esc = false;
            else if (c=='\\') esc = true;
            else if (c=='"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) { len = i + 1; break; }
        }
    }
    if (len == 0) return NULL;
    yyjson_read_err err;
    return ilsp_json_parse(body, len, parse_arena, &err);
}

/* ── Test 1 ──────────────────────────────────────────────────────────── */
static void test_uninit_rejects_nonlifecycle_request(void) {
    Harness h; harness_init(&h);

    const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,"
                       "\"method\":\"textDocument/didChange\",\"params\":{}}";
    dispatch(&h, body);
    harness_flush(&h);

    Iron_Arena pa = iron_arena_create(4 * 1024);
    yyjson_doc *d = parse_first(&h, &pa);
    TEST_ASSERT_NOT_NULL(d);
    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_val *err  = yyjson_obj_get(root, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_INT(-32002, (int)yyjson_get_int(yyjson_obj_get(err, "code")));
    iron_arena_free(&pa);

    /* State unchanged. */
    TEST_ASSERT_EQUAL_INT(ILSP_LIFECYCLE_UNINIT, h.server.lifecycle);

    harness_destroy(&h);
}

/* ── Test 2 ──────────────────────────────────────────────────────────── */
static void test_initialize_sets_state_to_initializing(void) {
    Harness h; harness_init(&h);

    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    dispatch(&h, body);
    harness_flush(&h);

    Iron_Arena pa = iron_arena_create(16 * 1024);
    yyjson_doc *d = parse_first(&h, &pa);
    TEST_ASSERT_NOT_NULL(d);
    yyjson_val *root = yyjson_doc_get_root(d);
    TEST_ASSERT_NOT_NULL(yyjson_obj_get(root, "result"));
    yyjson_val *result = yyjson_obj_get(root, "result");
    TEST_ASSERT_NOT_NULL(yyjson_obj_get(result, "capabilities"));
    iron_arena_free(&pa);

    TEST_ASSERT_EQUAL_INT(ILSP_LIFECYCLE_INITIALIZING, h.server.lifecycle);
    harness_destroy(&h);
}

/* ── Test 3 ──────────────────────────────────────────────────────────── */
static void test_initialized_transitions_to_running(void) {
    Harness h; harness_init(&h);

    /* initialize first. */
    dispatch(&h,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}");
    /* Then initialized (notification, no id). */
    dispatch(&h, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

    TEST_ASSERT_EQUAL_INT(ILSP_LIFECYCLE_RUNNING, h.server.lifecycle);

    harness_destroy(&h);
}

/* ── Test 4 ──────────────────────────────────────────────────────────── */
static void test_duplicate_initialize_rejected(void) {
    Harness h; harness_init(&h);

    dispatch(&h,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}");
    dispatch(&h, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    harness_flush(&h);

    /* Clear the sink before issuing the duplicate. */
    fflush(h.sink);
    size_t before_len = h.sink_len;

    dispatch(&h,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}");
    harness_flush(&h);

    /* Look only at bytes appended after the first initialize response. */
    const char *tail = h.sink_buf + before_len;
    size_t      tail_len = h.sink_len - before_len;
    const char *sep = memmem(tail, tail_len, "\r\n\r\n", 4);
    TEST_ASSERT_NOT_NULL(sep);
    const char *dup_body = sep + 4;
    size_t      dup_tail_len = tail_len - (size_t)(dup_body - tail);

    Iron_Arena pa = iron_arena_create(4 * 1024);
    yyjson_read_err err; memset(&err, 0, sizeof(err));
    /* Scan for the first balanced { ... } in dup_body. */
    size_t depth = 0, jlen = 0; bool in_str=false, esc=false;
    for (size_t i = 0; i < dup_tail_len; i++) {
        char c = dup_body[i];
        if (in_str) { if (esc) esc=false; else if (c=='\\') esc=true; else if (c=='"') in_str=false; continue; }
        if (c=='"') { in_str=true; continue; }
        if (c=='{') depth++;
        else if (c=='}') { depth--; if (depth==0) { jlen=i+1; break; } }
    }
    TEST_ASSERT_TRUE(jlen > 0);
    yyjson_doc *d = ilsp_json_parse(dup_body, jlen, &pa, &err);
    TEST_ASSERT_NOT_NULL(d);
    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_val *e    = yyjson_obj_get(root, "error");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(-32600, (int)yyjson_get_int(yyjson_obj_get(e, "code")));
    iron_arena_free(&pa);

    harness_destroy(&h);
}

/* ── Test 5 ──────────────────────────────────────────────────────────── */
static void test_shutdown_then_request_rejected(void) {
    Harness h; harness_init(&h);

    dispatch(&h,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}");
    dispatch(&h, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    dispatch(&h,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}");
    TEST_ASSERT_EQUAL_INT(ILSP_LIFECYCLE_SHUTDOWN, h.server.lifecycle);
    harness_flush(&h);
    size_t before_len = h.sink_len;

    dispatch(&h,
        "{\"jsonrpc\":\"2.0\",\"id\":3,"
        "\"method\":\"textDocument/didChange\",\"params\":{}}");
    harness_flush(&h);

    const char *tail = h.sink_buf + before_len;
    size_t      tail_len = h.sink_len - before_len;
    const char *sep = memmem(tail, tail_len, "\r\n\r\n", 4);
    TEST_ASSERT_NOT_NULL(sep);
    const char *jbody = sep + 4;
    size_t jbody_tail = tail_len - (size_t)(jbody - tail);

    size_t depth=0, jlen=0; bool in_str=false, esc=false;
    for (size_t i=0; i<jbody_tail; i++) {
        char c = jbody[i];
        if (in_str) { if (esc) esc=false; else if (c=='\\') esc=true; else if (c=='"') in_str=false; continue; }
        if (c=='"') { in_str=true; continue; }
        if (c=='{') depth++;
        else if (c=='}') { depth--; if (depth==0) { jlen=i+1; break; } }
    }
    TEST_ASSERT_TRUE(jlen > 0);

    Iron_Arena pa = iron_arena_create(4 * 1024);
    yyjson_read_err err; memset(&err, 0, sizeof(err));
    yyjson_doc *d = ilsp_json_parse(jbody, jlen, &pa, &err);
    TEST_ASSERT_NOT_NULL(d);
    yyjson_val *e = yyjson_obj_get(yyjson_doc_get_root(d), "error");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(-32600, (int)yyjson_get_int(yyjson_obj_get(e, "code")));
    iron_arena_free(&pa);

    harness_destroy(&h);
}

/* ── Test 6 ──────────────────────────────────────────────────────────── */
static void test_exit_without_shutdown_sets_exit_code(void) {
    /* Install the capture hook. */
    void (*orig)(int) = ilsp_exit_fn;
    ilsp_exit_fn = capture_exit;

    Harness h; harness_init(&h);

    /* exit from UNINIT: code should be 1 (protocol violation). */
    dispatch(&h, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");

    TEST_ASSERT_TRUE(g_exit_called);
    TEST_ASSERT_EQUAL_INT(1, g_captured_exit_code);

    /* Reset for second check: exit AFTER shutdown -> code 0. */
    g_exit_called = false;
    g_captured_exit_code = -1;
    harness_destroy(&h);

    Harness h2; harness_init(&h2);
    dispatch(&h2,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}");
    dispatch(&h2, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    dispatch(&h2,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\",\"params\":null}");
    TEST_ASSERT_EQUAL_INT(ILSP_LIFECYCLE_SHUTDOWN, h2.server.lifecycle);
    dispatch(&h2, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");

    TEST_ASSERT_TRUE(g_exit_called);
    TEST_ASSERT_EQUAL_INT(0, g_captured_exit_code);

    harness_destroy(&h2);

    ilsp_exit_fn = orig;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_uninit_rejects_nonlifecycle_request);
    RUN_TEST(test_initialize_sets_state_to_initializing);
    RUN_TEST(test_initialized_transitions_to_running);
    RUN_TEST(test_duplicate_initialize_rejected);
    RUN_TEST(test_shutdown_then_request_rejected);
    RUN_TEST(test_exit_without_shutdown_sets_exit_code);
    return UNITY_END();
}
