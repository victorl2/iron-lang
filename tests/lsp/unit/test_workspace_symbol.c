/* test_workspace_symbol -- Phase 3 Plan 03 Task 03 (NAV-08, D-11).
 *
 * Flipped from the Plan 01 Wave 0 stub. Drives
 * ilsp_facade_nav_workspace_symbol against a tmp workspace seeded
 * with a couple of .iron files.
 *
 * Six RUN_TESTs:
 *   1. fuzzy match surfaces the best-scoring candidate first.
 *   2. overlong query is rejected with an empty result (T-03-07).
 *   3. empty query / no workspace_index -> empty result.
 *   4. path-prefix filter narrows results via '/' split.
 *   5. modified open documents do not expose stale indexed symbols.
 *   6. open documents replace indexed symbols and report current ranges.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "support/posix_test.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── Tiny tmpdir helper ────────────────────────────────────────── */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

static void make_tmp_workspace(char *out_root, size_t cap) {
    char tmpl[] = IRON_TEST_TMP "/iron-ws-XXXXXX";
    char *dir = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(dir);
    strncpy(out_root, dir, cap - 1);
    out_root[cap - 1] = '\0';

    char tomlp[512], mainp[512], utilp[512];
    snprintf(tomlp, sizeof(tomlp), "%s/iron.toml", dir);
    snprintf(mainp, sizeof(mainp), "%s/main.iron",  dir);
    snprintf(utilp, sizeof(utilp), "%s/util.iron",  dir);

    write_file(tomlp,
        "[package]\nname=\"t\"\nversion=\"0.1.0\"\n");
    write_file(mainp,
        "pub func greeter() {}\n"
        "func reader()  {}\n"
        "func writer()  {}\n");
    write_file(utilp,
        "func helper() {}\n");

    char stalep[512];
    snprintf(stalep, sizeof(stalep), "%s/stale.iron", dir);
    write_file(stalep,
        "pub func old_symbol_with_long_name() {}\n");
}

static void rm_tree(const char *root) {
    /* Cheap recursive delete via system(); we're in a tmp dir of our
     * own creation, so fine. */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    int rc = system(cmd); (void)rc;
}

typedef struct {
    IronLsp_Server          server;
    IronLsp_WorkspaceIndex *wi;
    char                    root[512];
} fx_t;

static void fx_init(fx_t *f) {
    memset(f, 0, sizeof(*f));
    f->server.position_encoding = ILSP_ENC_UTF16;
    make_tmp_workspace(f->root, sizeof(f->root));
    f->wi = ilsp_workspace_index_create(f->root);
    TEST_ASSERT_NOT_NULL(f->wi);
    ilsp_workspace_index_warm_seed(f->wi, NULL);
    f->server.workspace_index = f->wi;
}

static void fx_destroy(fx_t *f) {
    if (f->server.documents) {
        for (ptrdiff_t i = 0; i < shlen(f->server.documents); i++) {
            ilsp_document_destroy(f->server.documents[i].value);
        }
        shfree(f->server.documents);
    }
    if (f->wi) ilsp_workspace_index_destroy(f->wi);
    rm_tree(f->root);
}

/* ── Test 01: fuzzy ranking surfaces best match first ─────────── */

static void test_fuzzy_returns_best_match(void) {
    fx_t f; fx_init(&f);

    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "gret", NULL, &arena,
                                       &out, &n);

    /* Must find at least one match; greeter scores highest because
     * subsequence g-r-e-t aligns tightly with "greeter". */
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1, n);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("greeter", out[0].name);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 02: overlong query rejected (T-03-07) ─────────────── */

static void test_query_too_long_returns_empty(void) {
    fx_t f; fx_init(&f);

    /* 512-char query. */
    char q[513];
    for (int i = 0; i < 512; i++) q[i] = 'a';
    q[512] = '\0';

    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, q, NULL, &arena, &out, &n);

    TEST_ASSERT_EQUAL_size_t(0, n);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 03: no workspace_index -> empty ─────────────────── */

static void test_no_workspace_index_empty(void) {
    IronLsp_Server server;
    memset(&server, 0, sizeof(server));
    server.position_encoding = ILSP_ENC_UTF16;
    server.workspace_index = NULL;

    Iron_Arena arena = iron_arena_create(4 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&server, "foo", NULL, &arena, &out, &n);

    TEST_ASSERT_EQUAL_size_t(0, n);
    iron_arena_free(&arena);
}

/* ── Test 04: path-prefix filter ─────────────────────────── */

static void test_path_prefix_filter(void) {
    fx_t f; fx_init(&f);

    /* Query "main/gret" should match greeter in main.iron only. */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "main/gret", NULL, &arena,
                                       &out, &n);

    /* All returned candidates must live in a path containing "main". */
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(out[i].uri);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out[i].uri, "main"),
            "path-prefix filter must restrict to main.iron");
    }

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* -- Test 05: edited open document suppresses stale index entry -- */

static void test_modified_open_document_suppresses_stale_symbols(void) {
    fx_t f; fx_init(&f);

    char uri[600];
    snprintf(uri, sizeof(uri), "file://%s/main.iron", f.root);
    const char *disk_text =
        "pub func greeter() {}\n"
        "func reader()  {}\n"
        "func writer()  {}\n";
    IronLsp_Document *doc = ilsp_document_create(
        uri, disk_text, strlen(disk_text), 1);
    TEST_ASSERT_NOT_NULL(doc);
    sh_new_strdup(f.server.documents);
    shput(f.server.documents, uri, doc);

    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "gret", NULL, &arena,
                                       &out, &n);
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(1, n);
    iron_arena_free(&arena);

    const char *edited_text = "pub func x() {}\n";
    TEST_ASSERT_TRUE(ilsp_document_apply_full_replace(
        doc, edited_text, strlen(edited_text), 2));

    arena = iron_arena_create(16 * 1024);
    out = NULL;
    n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "gret", NULL, &arena,
                                       &out, &n);
    TEST_ASSERT_EQUAL_size_t(0, n);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* Open buffers override disk-backed index entries (#832). */
static void test_open_document_overlays_saved_workspace_symbol(void) {
    fx_t f; fx_init(&f);

    char uri[600];
    snprintf(uri, sizeof(uri), "file://%s/stale.iron", f.root);
    const char *current = "pub func fresh() {}\n";
    IronLsp_Document *doc = ilsp_document_create(
        uri, current, strlen(current), 2);
    TEST_ASSERT_NOT_NULL(doc);
    sh_new_strdup(f.server.documents);
    shput(f.server.documents, uri, doc);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(
        &f.server, "old_symbol_with_long_name", NULL, &arena, &out, &n);
    TEST_ASSERT_EQUAL_size_t(0, n);

    iron_arena_free(&arena);
    arena = iron_arena_create(64 * 1024);
    out = NULL;
    n = 0;
    ilsp_facade_nav_workspace_symbol(
        &f.server, "fresh", NULL, &arena, &out, &n);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_STRING("fresh", out[0].name);
    TEST_ASSERT_EQUAL_UINT32(0, out[0].range.start.line);
    TEST_ASSERT_EQUAL_UINT32(0, out[0].range.end.line);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(
        (uint32_t)(strlen(current) - 1), out[0].range.end.character);

    iron_arena_free(&arena);
    fx_destroy(&f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzzy_returns_best_match);
    RUN_TEST(test_query_too_long_returns_empty);
    RUN_TEST(test_no_workspace_index_empty);
    RUN_TEST(test_path_prefix_filter);
    RUN_TEST(test_modified_open_document_suppresses_stale_symbols);
    RUN_TEST(test_open_document_overlays_saved_workspace_symbol);
    return UNITY_END();
}
