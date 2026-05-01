/* test_v3_visibility_workspace_symbol -- Phase 10 Plan 10-02 (VIS-02).
 *
 * Drives ilsp_facade_nav_workspace_symbol against a tmp workspace
 * seeded from the v3_visibility fixtures.  Asserts:
 *   1. fuzzy query "private_fn" returns 0 results (filter drops non-pub
 *      decls before the 256-cap, Pitfall 4).
 *   2. fuzzy query "public_fn" returns >=1 result (pub func passes).
 *   3. fuzzy query "Container" returns >=1 result (object decls have no
 *      is_pub axis -> default-true via ilsp_vis_is_public).
 *
 * Also direct-tests the ilsp_vis_is_public predicate against a synthetic
 * Iron_FuncDecl so the filter logic is verified independently of the
 * fuzzy matcher / score_decl pipeline.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {}

static char g_tmpdir[PATH_MAX] = {0};

static char *load_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(content);
    size_t n = fwrite(content, 1, len, f);
    fclose(f);
    return (n == len) ? 0 : -1;
}

static const char *find_fixture(char *out, size_t cap, const char *name) {
#ifdef IRON_SOURCE_TREE_ROOT
    snprintf(out, cap, "%s/tests/lsp/unit/v3_visibility/%s",
             IRON_SOURCE_TREE_ROOT, name);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return out; }
#endif
    snprintf(out, cap, "../tests/lsp/unit/v3_visibility/%s", name);
    FILE *f2 = fopen(out, "rb");
    if (f2) { fclose(f2); return out; }
    return NULL;
}

static int build_tmp_workspace(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/iron_v3vis_ws_XXXXXX");
    if (!mkdtemp(g_tmpdir)) return -1;

    char src[PATH_MAX], dst[PATH_MAX];
    if (!find_fixture(src, sizeof(src), "mod_a.iron")) return -1;
    char *a_text = load_file_text(src);
    if (!a_text) return -1;
    if (!find_fixture(src, sizeof(src), "mod_b.iron")) {
        free(a_text); return -1;
    }
    char *b_text = load_file_text(src);
    if (!b_text) { free(a_text); return -1; }

    snprintf(dst, sizeof(dst), "%s/mod_a.iron", g_tmpdir);
    write_file(dst, a_text);
    snprintf(dst, sizeof(dst), "%s/mod_b.iron", g_tmpdir);
    write_file(dst, b_text);

    char tomlp[PATH_MAX];
    snprintf(tomlp, sizeof(tomlp), "%s/iron.toml", g_tmpdir);
    write_file(tomlp,
        "[package]\nname=\"v3vis\"\nversion=\"0.1.0\"\n");

    free(a_text); free(b_text);
    return 0;
}

static void rm_rf_tmpdir(void) {
    if (g_tmpdir[0] == '\0') return;
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
    (void)!system(cmd);
    g_tmpdir[0] = '\0';
}

typedef struct {
    IronLsp_Server          server;
    IronLsp_WorkspaceIndex *wi;
} fx_t;

static void fx_init(fx_t *f) {
    memset(f, 0, sizeof(*f));
    f->server.position_encoding = ILSP_ENC_UTF16;
    f->wi = ilsp_workspace_index_create(g_tmpdir);
    TEST_ASSERT_NOT_NULL(f->wi);
    ilsp_workspace_index_warm_seed(f->wi, NULL);
    f->server.workspace_index = f->wi;
}

static void fx_destroy(fx_t *f) {
    if (f->wi) ilsp_workspace_index_destroy(f->wi);
    memset(f, 0, sizeof(*f));
}

/* ── Test 01: VIS-02 -- private_fn is excluded from workspace/symbol */

static void test_workspace_symbol_private_fn_excluded(void) {
    fx_t f; fx_init(&f);

    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "private_fn", NULL, &arena,
                                       &out, &n);

    /* No result entry should match the private_fn name. The filter is
     * applied INSIDE score_decl BEFORE arrput, so even a fuzzy match
     * against private_fn returns 0 entries. */
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_NULL(out[i].name);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(out[i].name, "private_fn"),
            "VIS-02: private_fn must NOT appear in workspace/symbol");
    }

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 02: VIS-02 -- public_fn IS included ─────────────────── */

static void test_workspace_symbol_public_fn_included(void) {
    fx_t f; fx_init(&f);

    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "public_fn", NULL, &arena,
                                       &out, &n);

    /* At least one match must be present and named "public_fn". */
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (out[i].name && strcmp(out[i].name, "public_fn") == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "VIS-02: pub func public_fn must appear in workspace/symbol");

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 03: VIS-02 -- Container object decl has no is_pub axis,  */
/* default-true (RESEARCH Conflict 3); should appear ────────────── */

static void test_workspace_symbol_object_decl_default_true(void) {
    fx_t f; fx_init(&f);

    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_WorkspaceSymbol *out = NULL;
    size_t n = 0;
    ilsp_facade_nav_workspace_symbol(&f.server, "Container", NULL, &arena,
                                       &out, &n);

    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (out[i].name && strcmp(out[i].name, "Container") == 0) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "VIS-02: ObjectDecl has no is_pub axis (default-true); must appear");

    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 04: predicate direct -- ilsp_vis_is_public 3-arm switch */

static void test_predicate_is_public_arms(void) {
    /* FuncDecl with is_private=true -> not public. */
    Iron_FuncDecl fd_priv = {0};
    fd_priv.kind = IRON_NODE_FUNC_DECL;
    fd_priv.is_private = true;
    TEST_ASSERT_FALSE(ilsp_vis_is_public((const Iron_Node *)&fd_priv));

    /* FuncDecl with is_private=false -> public. */
    Iron_FuncDecl fd_pub = {0};
    fd_pub.kind = IRON_NODE_FUNC_DECL;
    fd_pub.is_private = false;
    TEST_ASSERT_TRUE(ilsp_vis_is_public((const Iron_Node *)&fd_pub));

    /* ObjectDecl: no is_pub axis -> default-true. */
    Iron_ObjectDecl od = {0};
    od.kind = IRON_NODE_OBJECT_DECL;
    TEST_ASSERT_TRUE(ilsp_vis_is_public((const Iron_Node *)&od));

    /* NULL input -> false (defensive). */
    TEST_ASSERT_FALSE(ilsp_vis_is_public(NULL));
}

int main(void) {
    if (build_tmp_workspace() != 0) {
        fprintf(stderr, "Failed to build tmp workspace from fixtures\n");
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_predicate_is_public_arms);
    RUN_TEST(test_workspace_symbol_private_fn_excluded);
    RUN_TEST(test_workspace_symbol_public_fn_included);
    RUN_TEST(test_workspace_symbol_object_decl_default_true);
    int rc = UNITY_END();
    rm_rf_tmpdir();
    return rc;
}
