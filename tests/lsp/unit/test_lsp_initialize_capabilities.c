/* test_lsp_initialize_capabilities -- Phase 2 Plan 03 Task 02.
 *
 * Asserts that ServerCapabilities advertised in an initialize response
 * is derived exclusively from the compile-time handler table
 * (PITFALLS.md Pitfall 15 mitigation; CORE-06).
 *
 * Plan 03 registers only lifecycle handlers, all of which carry
 * capability = NULL -- so the built capabilities object should contain
 * ONLY the negotiated positionEncoding and no provider flags. When
 * Plans 04/05 add handlers whose capability field is non-NULL (e.g.
 * "textDocumentSync" on textDocument/didChange), those capabilities
 * will flip to "present" in this same test and the additions below
 * will pick them up automatically.
 *
 * Three kinds of assertion:
 *   1. Every `entry->capability != NULL` from the table appears in the
 *      response's capabilities object.
 *   2. Every key in the response's capabilities object (besides
 *      positionEncoding) corresponds to some entry->capability in the
 *      table (no drift -- we only advertise what we implement).
 *   3. A known-unregistered provider (hoverProvider) is absent. */
#include "unity.h"
#include "lsp/server/server.h"
#include "lsp/server/dispatch.h"
#include "lsp/server/capabilities.h"
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

extern IronLsp_DynRegister *ilsp_dyn_register_create(void);
extern void                 ilsp_dyn_register_destroy(IronLsp_DynRegister *r);

void setUp(void)    {}
void tearDown(void) {}

/* Drive initialize through the dispatcher and return the parsed
 * response doc (child-owned by the parse arena supplied by the
 * caller). Caller owns the sink buffer and the server. */
typedef struct {
    IronLsp_Server   server;
    IronLsp_Writer  *writer;
    FILE            *sink;
    char            *sink_buf;
    size_t           sink_len;
    Iron_Arena       arena;
} Harness;

static void harness_init(Harness *h) {
    memset(h, 0, sizeof(*h));
    h->sink = open_memstream(&h->sink_buf, &h->sink_len);
    h->writer = ilsp_writer_create(h->sink);
    h->arena = iron_arena_create(16 * 1024);
    h->server.lifecycle         = ILSP_LIFECYCLE_UNINIT;
    h->server.writer            = h->writer;
    h->server.cancels           = ilsp_cancel_registry_create();
    h->server.dyn_reg           = ilsp_dyn_register_create();
    h->server.position_encoding = ILSP_ENC_UTF16;
    atomic_store(&h->server.next_request_id, 1);
}

static void harness_destroy(Harness *h) {
    while (ilsp_writer_drain_one(h->writer)) { /* spin */ }
    ilsp_writer_destroy(h->writer);
    fclose(h->sink);
    free(h->sink_buf);
    ilsp_cancel_registry_destroy(h->server.cancels);
    ilsp_dyn_register_destroy(h->server.dyn_reg);
    iron_arena_free(&h->arena);
}

static yyjson_doc *send_initialize_and_parse(Harness *h, const char *body,
                                              Iron_Arena *pa) {
    ilsp_dispatch_route(&h->server, body, strlen(body), &h->arena);
    while (ilsp_writer_drain_one(h->writer)) { /* spin */ }
    fflush(h->sink);
    /* Skip framing. */
    const char *sep = memmem(h->sink_buf, h->sink_len, "\r\n\r\n", 4);
    TEST_ASSERT_NOT_NULL(sep);
    const char *jbody = sep + 4;
    size_t jlen = h->sink_len - (size_t)(jbody - h->sink_buf);
    yyjson_read_err err; memset(&err, 0, sizeof(err));
    yyjson_doc *d = ilsp_json_parse(jbody, jlen, pa, &err);
    TEST_ASSERT_NOT_NULL(d);
    return d;
}

/* ── Test 1: every table capability appears in the response ────────── */
static void test_table_capabilities_present_in_response(void) {
    Harness h; harness_init(&h);

    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    Iron_Arena pa = iron_arena_create(16 * 1024);
    yyjson_doc *d = send_initialize_and_parse(&h, body, &pa);

    yyjson_val *result = yyjson_obj_get(yyjson_doc_get_root(d), "result");
    TEST_ASSERT_NOT_NULL(result);
    yyjson_val *caps = yyjson_obj_get(result, "capabilities");
    TEST_ASSERT_NOT_NULL(caps);
    TEST_ASSERT_TRUE(yyjson_is_obj(caps));

    for (size_t i = 0; i < ilsp_handler_table_size; i++) {
        const char *cap = ilsp_handler_table[i].capability;
        if (!cap) continue;
        yyjson_val *v = yyjson_obj_get(caps, cap);
        TEST_ASSERT_NOT_NULL_MESSAGE(v,
            "advertised-in-table capability missing from response");
    }

    iron_arena_free(&pa);
    harness_destroy(&h);
}

/* ── Test 2: no drift -- every non-encoding response key is in the table ─ */
static void test_response_capabilities_all_registered(void) {
    Harness h; harness_init(&h);

    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    Iron_Arena pa = iron_arena_create(16 * 1024);
    yyjson_doc *d = send_initialize_and_parse(&h, body, &pa);

    yyjson_val *caps = yyjson_obj_get(
        yyjson_obj_get(yyjson_doc_get_root(d), "result"), "capabilities");
    TEST_ASSERT_NOT_NULL(caps);

    size_t kidx, kmax; yyjson_val *key, *val;
    yyjson_obj_foreach(caps, kidx, kmax, key, val) {
        const char *s = yyjson_get_str(key);
        TEST_ASSERT_NOT_NULL(s);
        if (strcmp(s, "positionEncoding") == 0) continue;

        bool found = false;
        for (size_t i = 0; i < ilsp_handler_table_size; i++) {
            const char *cap = ilsp_handler_table[i].capability;
            if (cap && strcmp(cap, s) == 0) { found = true; break; }
        }
        TEST_ASSERT_TRUE_MESSAGE(found,
            "capability advertised with no registered handler "
            "(PITFALLS.md #15 violation)");
    }

    iron_arena_free(&pa);
    harness_destroy(&h);
}

/* ── Test 3: unregistered provider is absent ────────────────────────── */
static void test_unregistered_provider_absent(void) {
    Harness h; harness_init(&h);

    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    Iron_Arena pa = iron_arena_create(16 * 1024);
    yyjson_doc *d = send_initialize_and_parse(&h, body, &pa);

    yyjson_val *caps = yyjson_obj_get(
        yyjson_obj_get(yyjson_doc_get_root(d), "result"), "capabilities");
    TEST_ASSERT_NOT_NULL(caps);

    /* Plan 03 has no hover handler registered -> capability must be absent. */
    TEST_ASSERT_NULL(yyjson_obj_get(caps, "hoverProvider"));
    TEST_ASSERT_NULL(yyjson_obj_get(caps, "definitionProvider"));
    TEST_ASSERT_NULL(yyjson_obj_get(caps, "referencesProvider"));

    iron_arena_free(&pa);
    harness_destroy(&h);
}

/* ── Test 4: serverInfo is populated ────────────────────────────────── */
static void test_server_info_populated(void) {
    Harness h; harness_init(&h);

    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"capabilities\":{}}}";
    Iron_Arena pa = iron_arena_create(16 * 1024);
    yyjson_doc *d = send_initialize_and_parse(&h, body, &pa);

    yyjson_val *info = yyjson_obj_get(
        yyjson_obj_get(yyjson_doc_get_root(d), "result"), "serverInfo");
    TEST_ASSERT_NOT_NULL(info);
    yyjson_val *name = yyjson_obj_get(info, "name");
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("ironls", yyjson_get_str(name));

    iron_arena_free(&pa);
    harness_destroy(&h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_table_capabilities_present_in_response);
    RUN_TEST(test_response_capabilities_all_registered);
    RUN_TEST(test_unregistered_provider_absent);
    RUN_TEST(test_server_info_populated);
    return UNITY_END();
}
