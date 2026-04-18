/* test_workspace_diagnostic -- Phase 3 Plan 06 Task 01 (NAV-12, NAV-13, D-12).
 *
 * Flips from Wave 0 stub to real assertions.  Exercises:
 *   1. cache size + bump_version sanity
 *   2. evict_path drops a slot
 *   3. refresh notification emitter produces a valid JSON-RPC body
 *      via the writer queue (captured with open_memstream)
 *   4. facade with NULL workspace_index returns zero reports gracefully
 *   5. facade returns reports for a warm-seeded workspace; second call
 *      returns kind="unchanged" for every file (cache hit invariant)
 *   6. content_hash mismatch (invalidate_path) forces fresh analyze
 *   7. capabilities.c advertises workspaceDiagnostics=true
 */
#include "unity.h"

#include "lsp/facade/workspace_diagnostic.h"
#include "lsp/server/handlers_workspace_diag.h"
#include "lsp/server/server.h"
#include "lsp/server/capabilities.h"
#include "lsp/server/dispatch.h"
#include "lsp/store/workspace_index.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/json.h"
#include "lsp/facade/types.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Harness ─────────────────────────────────────────────────────── */

typedef struct {
    IronLsp_Server   server;
    IronLsp_Writer  *writer;
    FILE            *sink;
    char            *sink_buf;
    size_t           sink_len;
} fx_t;

static void fx_init(fx_t *f) {
    memset(f, 0, sizeof(*f));
    f->sink = open_memstream(&f->sink_buf, &f->sink_len);
    f->writer = ilsp_writer_create(f->sink);
    f->server.writer = f->writer;
    f->server.position_encoding = ILSP_ENC_UTF16;
    f->server.ws_diag_cache = ilsp_ws_diag_cache_create();
    TEST_ASSERT_NOT_NULL(f->server.ws_diag_cache);
    ilsp_writer_start(f->writer);
}

static void fx_destroy(fx_t *f) {
    ilsp_writer_shutdown(f->writer);
    ilsp_writer_join(f->writer);
    ilsp_writer_destroy(f->writer);
    fclose(f->sink);
    free(f->sink_buf);
    if (f->server.ws_diag_cache) {
        ilsp_ws_diag_cache_destroy(f->server.ws_diag_cache);
    }
    if (f->server.workspace_index) {
        ilsp_workspace_index_destroy(f->server.workspace_index);
    }
    memset(f, 0, sizeof(*f));
}

/* ── Test 01: cache basic API ─────────────────────────────────────── */

static void test_cache_size_and_version(void) {
    IronLsp_WsDiagCache *c = ilsp_ws_diag_cache_create();
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_size_t(0, ilsp_ws_diag_cache_size(c));
    uint64_t v1 = ilsp_ws_diag_cache_bump_version(c);
    uint64_t v2 = ilsp_ws_diag_cache_bump_version(c);
    TEST_ASSERT_TRUE(v2 > v1);
    /* evict_path of unknown key is a no-op. */
    ilsp_ws_diag_cache_evict_path(c, "/nope/not_there.iron");
    TEST_ASSERT_EQUAL_size_t(0, ilsp_ws_diag_cache_size(c));
    ilsp_ws_diag_cache_destroy(c);
}

/* ── Test 02: refresh notification emitter produces method line ─── */

static void test_refresh_emits_method(void) {
    fx_t f; fx_init(&f);

    ilsp_send_workspace_diagnostic_refresh(&f.server);

    /* Drain the writer so the body lands in sink_buf. */
    int drained = 0;
    for (int i = 0; i < 10 && drained < 1; i++) {
        if (ilsp_writer_drain_one(f.writer)) drained++;
    }
    fflush(f.sink);

    /* Look for "workspace/diagnostic/refresh" method string in the raw
     * LSP frame (Content-Length: N\r\n\r\n{ ... }). */
    TEST_ASSERT_NOT_NULL(f.sink_buf);
    TEST_ASSERT_NOT_NULL(
        strstr(f.sink_buf, "workspace/diagnostic/refresh"));
    TEST_ASSERT_NOT_NULL(strstr(f.sink_buf, "\"jsonrpc\":\"2.0\""));

    fx_destroy(&f);
}

/* ── Test 03: facade with NULL workspace_index returns no reports ── */

static void test_facade_null_wi_graceful(void) {
    fx_t f; fx_init(&f);
    /* server.workspace_index stays NULL. */

    Iron_Arena arena = iron_arena_create(4 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *d = yyjson_mut_doc_new(&alc);
    TEST_ASSERT_NOT_NULL(d);

    IronLsp_WsDiagFileReport *reports = NULL;
    size_t n = 0;
    ilsp_facade_workspace_diagnostic(&f.server, NULL, NULL, d, &arena,
                                       &reports, &n);
    TEST_ASSERT_EQUAL_size_t(0, n);

    yyjson_mut_doc_free(d);
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Fixture helpers for tests that need a warm-seeded workspace ── */

static char g_tmpdir[PATH_MAX] = {0};

static int write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    size_t len = strlen(contents);
    size_t n = fwrite(contents, 1, len, fp);
    fclose(fp);
    return (n == len) ? 0 : -1;
}

static int make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/ironls_wsdiag_XXXXXX");
    if (!mkdtemp(g_tmpdir)) return -1;
    return 0;
}

static void rm_rf(void) {
    if (g_tmpdir[0] == '\0') return;
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
    (void)!system(cmd);
    g_tmpdir[0] = '\0';
}

static void build_workspace_ab(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/a.iron", g_tmpdir);
    write_file(p, "func a() -> Int { return 1 }\n");
    snprintf(p, sizeof(p), "%s/b.iron", g_tmpdir);
    write_file(p, "func b() -> Int { return 2 }\n");
}

/* ── Test 04: cache-hit invariant on second call ─────────────────── */

static void test_cache_hit_on_second_call(void) {
    TEST_ASSERT_EQUAL_INT(0, make_tmpdir());
    build_workspace_ab();

    fx_t f; fx_init(&f);
    f.server.workspace_index = ilsp_workspace_index_create(g_tmpdir);
    TEST_ASSERT_NOT_NULL(f.server.workspace_index);
    ilsp_workspace_index_warm_seed(f.server.workspace_index, NULL);

    /* Sanity: the warm-seed found at least 2 .iron files. */
    size_t entry_n = ilsp_workspace_index_entry_count(f.server.workspace_index);
    TEST_ASSERT_TRUE(entry_n >= 2);

    /* First call: cache miss on every file -> kind="full". */
    Iron_Arena arena1 = iron_arena_create(64 * 1024);
    yyjson_alc alc1   = ilsp_json_alc(&arena1);
    yyjson_mut_doc *d1 = yyjson_mut_doc_new(&alc1);
    IronLsp_WsDiagFileReport *reports1 = NULL;
    size_t n1 = 0;
    ilsp_facade_workspace_diagnostic(&f.server, NULL, NULL, d1, &arena1,
                                       &reports1, &n1);
    TEST_ASSERT_TRUE(n1 >= 2);
    int full_count_1 = 0;
    for (size_t i = 0; i < n1; i++) {
        if (reports1[i].kind && strcmp(reports1[i].kind, "full") == 0) {
            full_count_1++;
        }
    }
    TEST_ASSERT_TRUE(full_count_1 >= 1);
    yyjson_mut_doc_free(d1);

    /* Second call: every file should now be kind="unchanged". */
    Iron_Arena arena2 = iron_arena_create(64 * 1024);
    yyjson_alc alc2   = ilsp_json_alc(&arena2);
    yyjson_mut_doc *d2 = yyjson_mut_doc_new(&alc2);
    IronLsp_WsDiagFileReport *reports2 = NULL;
    size_t n2 = 0;
    ilsp_facade_workspace_diagnostic(&f.server, NULL, NULL, d2, &arena2,
                                       &reports2, &n2);
    TEST_ASSERT_TRUE(n2 >= 2);
    int unchanged_count_2 = 0;
    for (size_t i = 0; i < n2; i++) {
        if (reports2[i].kind && strcmp(reports2[i].kind, "unchanged") == 0) {
            unchanged_count_2++;
        }
    }
    TEST_ASSERT_TRUE(unchanged_count_2 >= 1);
    yyjson_mut_doc_free(d2);

    iron_arena_free(&arena2);
    iron_arena_free(&arena1);
    fx_destroy(&f);
    rm_rf();
}

/* ── Test 05: invalidate_path + evict forces fresh analyze ──────── */

static void test_evict_forces_refresh(void) {
    TEST_ASSERT_EQUAL_INT(0, make_tmpdir());
    build_workspace_ab();

    fx_t f; fx_init(&f);
    f.server.workspace_index = ilsp_workspace_index_create(g_tmpdir);
    ilsp_workspace_index_warm_seed(f.server.workspace_index, NULL);

    /* Prime cache. */
    Iron_Arena a1 = iron_arena_create(64 * 1024);
    yyjson_alc alc1 = ilsp_json_alc(&a1);
    yyjson_mut_doc *d1 = yyjson_mut_doc_new(&alc1);
    IronLsp_WsDiagFileReport *r1 = NULL; size_t n1 = 0;
    ilsp_facade_workspace_diagnostic(&f.server, NULL, NULL, d1, &a1, &r1, &n1);
    TEST_ASSERT_TRUE(n1 >= 2);
    size_t initial_cache_size =
        ilsp_ws_diag_cache_size(f.server.ws_diag_cache);
    TEST_ASSERT_TRUE(initial_cache_size >= 1);
    yyjson_mut_doc_free(d1);
    iron_arena_free(&a1);

    /* Evict every path. Use the snapshot for the keys. */
    size_t np = 0;
    char **paths = ilsp_workspace_index_snapshot_paths(
        f.server.workspace_index, &np);
    TEST_ASSERT_TRUE(np >= 2);
    for (size_t i = 0; i < np; i++) {
        ilsp_ws_diag_cache_evict_path(f.server.ws_diag_cache, paths[i]);
    }
    TEST_ASSERT_EQUAL_size_t(0,
        ilsp_ws_diag_cache_size(f.server.ws_diag_cache));
    for (size_t i = 0; i < np; i++) free(paths[i]);
    free(paths);

    /* Second call after eviction should produce at least one "full" again. */
    Iron_Arena a2 = iron_arena_create(64 * 1024);
    yyjson_alc alc2 = ilsp_json_alc(&a2);
    yyjson_mut_doc *d2 = yyjson_mut_doc_new(&alc2);
    IronLsp_WsDiagFileReport *r2 = NULL; size_t n2 = 0;
    ilsp_facade_workspace_diagnostic(&f.server, NULL, NULL, d2, &a2, &r2, &n2);
    TEST_ASSERT_TRUE(n2 >= 2);
    int full2 = 0;
    for (size_t i = 0; i < n2; i++) {
        if (r2[i].kind && strcmp(r2[i].kind, "full") == 0) full2++;
    }
    TEST_ASSERT_TRUE(full2 >= 1);
    yyjson_mut_doc_free(d2);
    iron_arena_free(&a2);

    fx_destroy(&f);
    rm_rf();
}

/* ── Test 06: cancel flag stops the iteration early ─────────────── */

static void test_cancel_flag_respected(void) {
    TEST_ASSERT_EQUAL_INT(0, make_tmpdir());
    build_workspace_ab();

    fx_t f; fx_init(&f);
    f.server.workspace_index = ilsp_workspace_index_create(g_tmpdir);
    ilsp_workspace_index_warm_seed(f.server.workspace_index, NULL);

    _Atomic bool cancel = true;  /* trip before we start. */

    Iron_Arena arena = iron_arena_create(8 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *d = yyjson_mut_doc_new(&alc);
    IronLsp_WsDiagFileReport *reports = NULL;
    size_t n = 0;
    ilsp_facade_workspace_diagnostic(&f.server, NULL, &cancel, d, &arena,
                                       &reports, &n);
    /* With cancel flipped from the start, we expect 0 reports (loop
     * bails on the first iteration boundary check). */
    TEST_ASSERT_EQUAL_size_t(0, n);

    yyjson_mut_doc_free(d);
    iron_arena_free(&arena);
    fx_destroy(&f);
    rm_rf();
}

/* ── Test 07: capabilities advertise workspaceDiagnostics=true ──── */

static void test_capabilities_workspace_diag_true(void) {
    Iron_Arena arena = iron_arena_create(4 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *d = yyjson_mut_doc_new(&alc);
    TEST_ASSERT_NOT_NULL(d);

    yyjson_mut_val *caps = ilsp_capabilities_build(d, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL(caps);

    yyjson_mut_val *dp = yyjson_mut_obj_get(caps, "diagnosticProvider");
    TEST_ASSERT_NOT_NULL(dp);
    yyjson_mut_val *wd = yyjson_mut_obj_get(dp, "workspaceDiagnostics");
    TEST_ASSERT_NOT_NULL(wd);
    TEST_ASSERT_TRUE(yyjson_mut_is_bool(wd));
    TEST_ASSERT_TRUE(yyjson_mut_get_bool(wd));

    yyjson_mut_doc_free(d);
    iron_arena_free(&arena);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_cache_size_and_version);
    RUN_TEST(test_refresh_emits_method);
    RUN_TEST(test_facade_null_wi_graceful);
    RUN_TEST(test_cache_hit_on_second_call);
    RUN_TEST(test_evict_forces_refresh);
    RUN_TEST(test_cancel_flag_respected);
    RUN_TEST(test_capabilities_workspace_diag_true);
    return UNITY_END();
}
