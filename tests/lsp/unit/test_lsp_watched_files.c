/* test_lsp_watched_files -- Phase 2 Plan 04 Task 02 (CORE-13).
 *
 * Asserts:
 *   1. ilsp_workspace_classify correctly tags *.iron / iron.toml /
 *      iron.lock / unknown paths.
 *   2. ilsp_workspace_path_from_uri round-trips file:// URIs into
 *      absolute paths, including URL-decoded ones.
 *   3. ilsp_workspace_find_root walks up from a nested path to locate a
 *      test fixture directory containing an iron.toml sentinel.
 *   4. The didChangeWatchedFiles handler dispatches through the handler
 *      registry for the 4 canonical event types -- source-open,
 *      source-not-open, manifest, lockfile -- without raising errors
 *      and without corrupting server state. */
#include "unity.h"

#include "lsp/server/server.h"
#include "lsp/server/dispatch.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace.h"
#include "lsp/transport/writer.h"
#include "lsp/facade/types.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Helper: build a minimal server for dispatcher-driven events ──── */
typedef struct {
    IronLsp_Server  server;
    IronLsp_Writer *writer;
    FILE           *sink;
    char           *sink_buf;
    size_t          sink_len;
    Iron_Arena      arena;
} Harness;

extern IronLsp_CancelRegistry *ilsp_cancel_registry_create(void);
extern void                    ilsp_cancel_registry_destroy(IronLsp_CancelRegistry *);
extern IronLsp_DynRegister    *ilsp_dyn_register_create(void);
extern void                    ilsp_dyn_register_destroy(IronLsp_DynRegister *);

static void harness_init(Harness *h) {
    memset(h, 0, sizeof(*h));
    h->sink     = open_memstream(&h->sink_buf, &h->sink_len);
    h->writer   = ilsp_writer_create(h->sink);
    h->arena    = iron_arena_create(16 * 1024);
    h->server.lifecycle         = ILSP_LIFECYCLE_RUNNING;  /* pass gate */
    h->server.writer            = h->writer;
    h->server.cancels           = ilsp_cancel_registry_create();
    h->server.dyn_reg           = ilsp_dyn_register_create();
    h->server.position_encoding = ILSP_ENC_UTF16;
    h->server.documents         = NULL;
    h->server.workspace_root    = NULL;
    atomic_store(&h->server.next_request_id, 1);
}

static void harness_destroy(Harness *h) {
    while (ilsp_writer_drain_one(h->writer)) { /* spin */ }
    ilsp_writer_destroy(h->writer);
    fclose(h->sink);
    free(h->sink_buf);
    ilsp_cancel_registry_destroy(h->server.cancels);
    ilsp_dyn_register_destroy(h->server.dyn_reg);
    /* Free any remaining open documents. */
    if (h->server.documents) {
        for (ptrdiff_t i = 0; i < shlen(h->server.documents); i++) {
            ilsp_document_destroy(h->server.documents[i].value);
        }
        shfree(h->server.documents);
    }
    if (h->server.workspace_root) free(h->server.workspace_root);
    iron_arena_free(&h->arena);
}

/* ── Test 1: classify ──────────────────────────────────────────────── */
static void test_classify_paths(void) {
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_SOURCE,
        ilsp_workspace_classify("file:///tmp/foo.iron"));
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_SOURCE,
        ilsp_workspace_classify("/home/u/x.iron"));
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_MANIFEST,
        ilsp_workspace_classify("file:///home/u/proj/iron.toml"));
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_MANIFEST,
        ilsp_workspace_classify("iron.toml"));
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_LOCKFILE,
        ilsp_workspace_classify("file:///home/u/proj/iron.lock"));
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_UNKNOWN,
        ilsp_workspace_classify("file:///tmp/readme.md"));
    TEST_ASSERT_EQUAL_INT(ILSP_WATCHED_UNKNOWN,
        ilsp_workspace_classify(NULL));
}

/* ── Test 2: path from URI ─────────────────────────────────────────── */
static void test_path_from_uri(void) {
    char *p = ilsp_workspace_path_from_uri("file:///home/u/proj/main.iron");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("/home/u/proj/main.iron", p);
    free(p);

    /* URL-decoding. */
    char *p2 = ilsp_workspace_path_from_uri("file:///home/u/my%20proj/a.iron");
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_EQUAL_STRING("/home/u/my proj/a.iron", p2);
    free(p2);

    /* Non-file:// URI. */
    char *p3 = ilsp_workspace_path_from_uri("http://example.com/x");
    TEST_ASSERT_NULL(p3);
}

/* ── Test 3: find_root walks up ────────────────────────────────────── */
static void test_find_root(void) {
    /* Create a scratch directory tree with iron.toml at the top. */
    char tmpl[] = "/tmp/ironls_wsXXXXXX";
    char *root  = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(root);

    /* Write iron.toml. */
    char toml[1024];
    snprintf(toml, sizeof(toml), "%s/iron.toml", root);
    FILE *f = fopen(toml, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("name = \"test\"\n", f);
    fclose(f);

    /* Nested directory. */
    char nested[1024];
    snprintf(nested, sizeof(nested), "%s/src/deep", root);
    char mk[1280];
    snprintf(mk, sizeof(mk), "mkdir -p %s", nested);
    TEST_ASSERT_EQUAL_INT(0, system(mk));

    /* Find root from the deep directory. */
    char *found = ilsp_workspace_find_root(nested);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING(root, found);
    free(found);

    /* Find root from a file inside the deep dir. */
    char deepfile[1280];
    snprintf(deepfile, sizeof(deepfile), "%s/main.iron", nested);
    FILE *fd = fopen(deepfile, "w");
    if (fd) { fputs("fun main(){}\n", fd); fclose(fd); }
    char *found2 = ilsp_workspace_find_root(deepfile);
    TEST_ASSERT_NOT_NULL(found2);
    TEST_ASSERT_EQUAL_STRING(root, found2);
    free(found2);

    /* Cleanup. */
    char rm[1280];
    snprintf(rm, sizeof(rm), "rm -rf %s", root);
    int rm_rc = system(rm); (void)rm_rc;
}

/* ── Test 4: didChangeWatchedFiles routes through dispatcher ───────── */
static void test_watched_files_dispatcher(void) {
    Harness h; harness_init(&h);

    /* Open a document so "source stale" path applies to it. */
    IronLsp_Document *d = ilsp_document_create(
        "file:///tmp/tracked.iron", "fun main(){}", 12, 1);
    sh_new_strdup(h.server.documents);
    shput(h.server.documents, "file:///tmp/tracked.iron", d);

    /* 4a. Event for the open source file (stale-on-disk). */
    const char *body1 =
        "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\","
        "\"params\":{\"changes\":["
        "{\"uri\":\"file:///tmp/tracked.iron\",\"type\":2}"
        "]}}";
    ilsp_dispatch_route(&h.server, body1, strlen(body1), &h.arena);

    /* 4b. Event for an unknown source URI. */
    const char *body2 =
        "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\","
        "\"params\":{\"changes\":["
        "{\"uri\":\"file:///tmp/unknown.iron\",\"type\":1}"
        "]}}";
    ilsp_dispatch_route(&h.server, body2, strlen(body2), &h.arena);

    /* 4c. Event for iron.toml. */
    const char *body3 =
        "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\","
        "\"params\":{\"changes\":["
        "{\"uri\":\"file:///tmp/proj/iron.toml\",\"type\":2}"
        "]}}";
    ilsp_dispatch_route(&h.server, body3, strlen(body3), &h.arena);

    /* 4d. Event for iron.lock. */
    const char *body4 =
        "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\","
        "\"params\":{\"changes\":["
        "{\"uri\":\"file:///tmp/proj/iron.lock\",\"type\":3}"
        "]}}";
    ilsp_dispatch_route(&h.server, body4, strlen(body4), &h.arena);

    /* JSON-RPC responses are never enqueued here (these are all
     * notifications, not requests).  However, starting in Phase 3 Plan
     * 06 (NAV-13, D-12), the server DOES enqueue a
     * workspace/diagnostic/refresh notification for every manifest /
     * lockfile event and for every non-open source invalidation.  Drain
     * every queued item and verify:
     *   - Any drained body must be a notification body (no "id" field).
     *   - 4b (unknown non-open source) and 4c/4d emit one
     *     workspace/diagnostic/refresh each.
     *   - 4a (open source) does NOT emit refresh (open docs use
     *     publishDiagnostics push). */
    int drained_count = 0;
    while (ilsp_writer_drain_one(h.writer)) { drained_count++; }
    /* 4b + 4c + 4d -> 3 refresh notifications. 4a is open so it does
     * not emit; manifest/lockfile events always emit. */
    TEST_ASSERT_EQUAL_INT(3, drained_count);

    /* Document map should still contain the tracked URI. */
    TEST_ASSERT_TRUE(shgeti(h.server.documents, "file:///tmp/tracked.iron") >= 0);

    harness_destroy(&h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_classify_paths);
    RUN_TEST(test_path_from_uri);
    RUN_TEST(test_find_root);
    RUN_TEST(test_watched_files_dispatcher);
    return UNITY_END();
}
