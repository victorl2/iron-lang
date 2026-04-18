/* test_lsp_dynamic_registration -- Phase 2 Plan 03 Task 02.
 *
 * Asserts the shape of the outbound `client/registerCapability` request
 * emitted by ilsp_dyn_register_watched_files (CORE-08): an array of
 * watchers for the three globs (wildcards slash *.iron, wildcards slash
 * iron.toml, wildcards slash iron.lock) with watch kind 7
 * (Create | Change | Delete). */
#include "unity.h"
#include "lsp/server/server.h"
#include "lsp/server/dyn_register.h"
#include "lsp/server/cancel.h"
#include "lsp/server/lifecycle.h"
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

/* Drive the registration, drain the sink, return the parsed request. */
static yyjson_doc *capture_registration(IronLsp_Server *s, IronLsp_Writer *w,
                                        FILE *sink, char **sink_buf,
                                        size_t *sink_len, Iron_Arena *pa) {
    ilsp_dyn_register_watched_files(s);
    while (ilsp_writer_drain_one(w)) { /* spin */ }
    fflush(sink);

    const char *sep = memmem(*sink_buf, *sink_len, "\r\n\r\n", 4);
    TEST_ASSERT_NOT_NULL(sep);
    const char *jbody = sep + 4;
    size_t jlen = *sink_len - (size_t)(jbody - *sink_buf);
    yyjson_read_err err; memset(&err, 0, sizeof(err));
    yyjson_doc *d = ilsp_json_parse(jbody, jlen, pa, &err);
    TEST_ASSERT_NOT_NULL(d);
    return d;
}

/* ── Test 1: method is client/registerCapability ────────────────────── */
static void test_outbound_method(void) {
    char *buf = NULL; size_t len = 0;
    FILE *sink = open_memstream(&buf, &len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    IronLsp_Server s; memset(&s, 0, sizeof(s));
    s.writer  = w;
    s.dyn_reg = ilsp_dyn_register_create();
    atomic_store(&s.next_request_id, 42);

    Iron_Arena pa = iron_arena_create(8 * 1024);
    yyjson_doc *d = capture_registration(&s, w, sink, &buf, &len, &pa);

    yyjson_val *root = yyjson_doc_get_root(d);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("client/registerCapability",
        yyjson_get_str(yyjson_obj_get(root, "method")));
    TEST_ASSERT_EQUAL_STRING("2.0",
        yyjson_get_str(yyjson_obj_get(root, "jsonrpc")));
    /* id should be the pre-increment counter value (42). */
    yyjson_val *id = yyjson_obj_get(root, "id");
    TEST_ASSERT_NOT_NULL(id);
    TEST_ASSERT_EQUAL_UINT(42, (unsigned int)yyjson_get_uint(id));

    iron_arena_free(&pa);
    ilsp_dyn_register_destroy(s.dyn_reg);
    ilsp_writer_destroy(w);
    fclose(sink);
    free(buf);
}

/* ── Test 2: watchers array has 3 entries with expected globs ───────── */
static void test_watchers_payload(void) {
    char *buf = NULL; size_t len = 0;
    FILE *sink = open_memstream(&buf, &len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    IronLsp_Server s; memset(&s, 0, sizeof(s));
    s.writer  = w;
    s.dyn_reg = ilsp_dyn_register_create();
    atomic_store(&s.next_request_id, 1);

    Iron_Arena pa = iron_arena_create(8 * 1024);
    yyjson_doc *d = capture_registration(&s, w, sink, &buf, &len, &pa);

    yyjson_val *root = yyjson_doc_get_root(d);
    yyjson_val *params = yyjson_obj_get(root, "params");
    TEST_ASSERT_NOT_NULL(params);
    yyjson_val *regs = yyjson_obj_get(params, "registrations");
    TEST_ASSERT_NOT_NULL(regs);
    TEST_ASSERT_TRUE(yyjson_is_arr(regs));
    TEST_ASSERT_EQUAL_UINT(1, yyjson_arr_size(regs));

    yyjson_val *reg0 = yyjson_arr_get(regs, 0);
    TEST_ASSERT_EQUAL_STRING("workspace/didChangeWatchedFiles",
        yyjson_get_str(yyjson_obj_get(reg0, "method")));

    yyjson_val *ropts = yyjson_obj_get(reg0, "registerOptions");
    TEST_ASSERT_NOT_NULL(ropts);
    yyjson_val *watchers = yyjson_obj_get(ropts, "watchers");
    TEST_ASSERT_NOT_NULL(watchers);
    TEST_ASSERT_TRUE(yyjson_is_arr(watchers));
    TEST_ASSERT_EQUAL_UINT(3, yyjson_arr_size(watchers));

    /* Collect globs. */
    bool found_iron = false, found_toml = false, found_lock = false;
    size_t i, m; yyjson_val *wv;
    yyjson_arr_foreach(watchers, i, m, wv) {
        const char *g = yyjson_get_str(yyjson_obj_get(wv, "globPattern"));
        TEST_ASSERT_NOT_NULL(g);
        if (strcmp(g, "**/*.iron")    == 0) found_iron = true;
        if (strcmp(g, "**/iron.toml") == 0) found_toml = true;
        if (strcmp(g, "**/iron.lock") == 0) found_lock = true;
        /* Kind 7 = Create | Change | Delete. */
        TEST_ASSERT_EQUAL_INT(7, (int)yyjson_get_int(yyjson_obj_get(wv, "kind")));
    }
    TEST_ASSERT_TRUE(found_iron);
    TEST_ASSERT_TRUE(found_toml);
    TEST_ASSERT_TRUE(found_lock);

    iron_arena_free(&pa);
    ilsp_dyn_register_destroy(s.dyn_reg);
    ilsp_writer_destroy(w);
    fclose(sink);
    free(buf);
}

/* ── Test 3: registration id is stable ──────────────────────────────── */
static void test_registration_id_stable(void) {
    char *buf = NULL; size_t len = 0;
    FILE *sink = open_memstream(&buf, &len);
    IronLsp_Writer *w = ilsp_writer_create(sink);

    IronLsp_Server s; memset(&s, 0, sizeof(s));
    s.writer  = w;
    s.dyn_reg = ilsp_dyn_register_create();
    atomic_store(&s.next_request_id, 1);

    Iron_Arena pa = iron_arena_create(8 * 1024);
    yyjson_doc *d = capture_registration(&s, w, sink, &buf, &len, &pa);

    yyjson_val *reg0 = yyjson_arr_get(
        yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(d), "params"),
                       "registrations"),
        0);
    TEST_ASSERT_EQUAL_STRING("ironls-watch-1",
        yyjson_get_str(yyjson_obj_get(reg0, "id")));

    iron_arena_free(&pa);
    ilsp_dyn_register_destroy(s.dyn_reg);
    ilsp_writer_destroy(w);
    fclose(sink);
    free(buf);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_outbound_method);
    RUN_TEST(test_watchers_payload);
    RUN_TEST(test_registration_id_stable);
    return UNITY_END();
}
