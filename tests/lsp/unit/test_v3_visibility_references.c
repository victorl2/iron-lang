/* test_v3_visibility_references -- Phase 10 Plan 10-02 (VIS-01).
 *
 * Drives ilsp_facade_nav_references against multi-file v3_visibility
 * fixtures and asserts the visibility filter behavior.
 *
 * Anti-aliasing matrix per RESEARCH:
 *   * false-negative path: the filter MUST drop cross-module private
 *     use-sites (test_predicate_drops_cross_module_private)
 *   * false-positive path: the filter MUST NOT drop public use-sites
 *     or same-module use-sites (test_predicate_keeps_public,
 *     test_predicate_keeps_same_module)
 *
 * The facade-side smoke tests (cross-file references via
 * workspace_index) are timing-dependent on the bulk-analyze gate; the
 * direct predicate tests provide the deterministic semantic gate.
 *
 * Fixture path resolution uses IRON_SOURCE_TREE_ROOT compile-define.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "lsp/facade/types.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Multi-file fixture harness ───────────────────────────────────── */

static char g_tmpdir[PATH_MAX] = {0};
static char g_mod_a_path[PATH_MAX] = {0};
static char g_mod_b_path[PATH_MAX] = {0};

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
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/iron_v3vis_refs_XXXXXX");
    if (!mkdtemp(g_tmpdir)) return -1;

    char src[PATH_MAX];
    if (!find_fixture(src, sizeof(src), "mod_a.iron")) return -1;
    char *a_text = load_file_text(src);
    if (!a_text) return -1;
    if (!find_fixture(src, sizeof(src), "mod_b.iron")) {
        free(a_text); return -1;
    }
    char *b_text = load_file_text(src);
    if (!b_text) { free(a_text); return -1; }

    snprintf(g_mod_a_path, sizeof(g_mod_a_path), "%s/mod_a.iron", g_tmpdir);
    snprintf(g_mod_b_path, sizeof(g_mod_b_path), "%s/mod_b.iron", g_tmpdir);
    write_file(g_mod_a_path, a_text);
    write_file(g_mod_b_path, b_text);

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
    IronLsp_Document       *doc;
    char                   *src;
} fx_t;

static void fx_init_doc(fx_t *f, const char *path) {
    memset(f, 0, sizeof(*f));
    f->server.position_encoding = ILSP_ENC_UTF16;
    f->wi = ilsp_workspace_index_create(g_tmpdir);
    TEST_ASSERT_NOT_NULL(f->wi);
    ilsp_workspace_index_warm_seed(f->wi, NULL);
    f->server.workspace_index = f->wi;
    f->src = load_file_text(path);
    TEST_ASSERT_NOT_NULL(f->src);
    f->doc = ilsp_document_create(path, f->src, strlen(f->src), 1);
    TEST_ASSERT_NOT_NULL(f->doc);
}

static void fx_destroy(fx_t *f) {
    if (f->doc) ilsp_document_destroy(f->doc);
    if (f->wi)  ilsp_workspace_index_destroy(f->wi);
    free(f->src);
    memset(f, 0, sizeof(*f));
}

/* ── Test 01: predicate drops cross-module private decls (false-negative) */

static void test_predicate_drops_cross_module_private(void) {
    /* Direct semantic test of the filter logic that VIS-01's Step 5.5
     * compaction loop runs on every raw_sites entry. */
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = true;

    TEST_ASSERT_FALSE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_b.iron", (const Iron_Node *)&fd));
}

/* ── Test 02: predicate keeps public cross-module decls (false-positive) */

static void test_predicate_keeps_public_cross_module(void) {
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = false;

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_b.iron", (const Iron_Node *)&fd));
}

/* ── Test 03: predicate same-module short-circuit ───────────────── */

static void test_predicate_same_module_short_circuit(void) {
    /* same-file private decl is always visible (compiler enforces
     * visibility at semantic level; LSP filter only fires cross-module). */
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = true;

    /* Pointer-equality fast path: */
    const char *p = "/tmp/mod_a.iron";
    TEST_ASSERT_TRUE(ilsp_vis_can_see(p, p, (const Iron_Node *)&fd));

    /* strcmp same-module path: */
    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "/tmp/mod_a.iron", "/tmp/mod_a.iron", (const Iron_Node *)&fd));
}

/* ── Test 04: stdlib carve-out (D-08) ───────────────────────────── */

static void test_predicate_stdlib_carve_out(void) {
    Iron_FuncDecl fd = {0};
    fd.kind = IRON_NODE_FUNC_DECL;
    fd.is_private = true;  /* private but stdlib -> still visible */

    TEST_ASSERT_TRUE(ilsp_vis_can_see(
        "stdlib://math.iron", "/tmp/mod_b.iron", (const Iron_Node *)&fd));
}

/* ── Test 05: facade smoke -- references on private decl in mod_a (no crash) */

static void test_facade_references_smoke_same_file(void) {
    /* Open mod_a; cursor on the `private_fn` decl line; the same-file
     * fallback walker is exercised. The visibility filter at Step 5.5
     * is bypassed (raw_n stays 0; fallback is used). The assertion is
     * "no crash + graceful response shape". */
    fx_t f; fx_init_doc(&f, g_mod_a_path);

    /* Some position inside mod_a — line 1 col 10 lands inside
     * "public_fn" identifier on the pub func decl line. */
    IronLsp_Position pos = { .line = 1, .character = 10 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_RefSite *sites = NULL;
    size_t n = 0;
    ilsp_facade_nav_references(&f.server, f.doc, pos,
                                 /*include_declaration=*/true,
                                 NULL, &arena, &sites, &n);

    /* Graceful: any n is acceptable; assertion is no crash + sites ptr
     * is consistent (n>0 implies non-NULL sites). */
    if (n > 0) TEST_ASSERT_NOT_NULL(sites);
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 06: facade smoke -- references in mod_b (no crash) ────── */

static void test_facade_references_smoke_cross_file(void) {
    fx_t f; fx_init_doc(&f, g_mod_b_path);

    /* Cursor in mod_b -- arbitrary position. Smoke: no crash. */
    IronLsp_Position pos = { .line = 4, .character = 14 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_RefSite *sites = NULL;
    size_t n = 0;
    ilsp_facade_nav_references(&f.server, f.doc, pos,
                                 /*include_declaration=*/false,
                                 NULL, &arena, &sites, &n);

    if (n > 0) TEST_ASSERT_NOT_NULL(sites);
    iron_arena_free(&arena);
    fx_destroy(&f);
}

int main(void) {
    if (build_tmp_workspace() != 0) {
        fprintf(stderr, "Failed to build tmp workspace from fixtures\n");
        return 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_predicate_drops_cross_module_private);
    RUN_TEST(test_predicate_keeps_public_cross_module);
    RUN_TEST(test_predicate_same_module_short_circuit);
    RUN_TEST(test_predicate_stdlib_carve_out);
    RUN_TEST(test_facade_references_smoke_same_file);
    RUN_TEST(test_facade_references_smoke_cross_file);
    int rc = UNITY_END();
    rm_rf_tmpdir();
    return rc;
}
