/* test_workspace_symbol -- Phase 3 Plan 03 Task 03 (NAV-08, D-11).
 *
 * Flipped from the Plan 01 Wave 0 stub. Drives
 * ilsp_facade_nav_workspace_symbol against a tmp workspace seeded
 * with a couple of .iron files.
 *
 * Four RUN_TESTs:
 *   1. fuzzy match surfaces the best-scoring candidate first.
 *   2. overlong query is rejected with an empty result (T-03-07).
 *   3. empty query / no workspace_index -> empty result.
 *   4. path-prefix filter narrows results via '/' split.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "util/arena.h"

#include <errno.h>
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

/* ── Tiny tmpdir helper ────────────────────────────────────────── */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(content, f);
    fclose(f);
}

static void make_tmp_workspace(char *out_root, size_t cap) {
    char tmpl[] = "/tmp/iron-ws-XXXXXX";
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
        "func greeter() {}\n"
        "func reader()  {}\n"
        "func writer()  {}\n");
    write_file(utilp,
        "func helper() {}\n");
}

static void rm_tree(const char *root) {
    /* Cheap recursive delete via system(); we're in a tmp dir of our
     * own creation, so fine. */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    (void)system(cmd);
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fuzzy_returns_best_match);
    RUN_TEST(test_query_too_long_returns_empty);
    RUN_TEST(test_no_workspace_index_empty);
    RUN_TEST(test_path_prefix_filter);
    return UNITY_END();
}
