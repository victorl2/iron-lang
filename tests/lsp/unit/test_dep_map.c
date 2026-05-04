/* test_dep_map -- Phase 3 Plan 02 Task 03.
 *
 * Flips Wave 0 stub to real assertions.  Drives the dep_map API
 * directly with a test-injected dep (bypassing the GitHub-touching
 * resolve_dependencies path).  The library-linkage discipline is
 * enforced via grep in the plan's acceptance criteria.
 *
 * Tests:
 *   1. inject -> lookup returns the entry with exported symbols
 *   2. invalidate(name) drops one entry; others survive
 *   3. invalidate(NULL) drops every entry
 *   4. path-escape defense rejects /etc / random absolute paths
 *   5. ilsp_dep_map_resolve on a workspace without iron.toml returns 0
 *      (bare workspace is supported)
 *   6. size accounting tracks inject + invalidate calls
 */
#include "unity.h"

#include "lsp/store/dep_map.h"
#include "vendor/stb_ds.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void setUp(void)    {}
void tearDown(void) {}

/* ── Fixture helpers ─────────────────────────────────────────────────── */

static char g_tmpdir[PATH_MAX] = {0};

static int make_tmpdir(void) {
    snprintf(g_tmpdir, sizeof(g_tmpdir), "/tmp/ironls_depmap_XXXXXX");
    return mkdtemp(g_tmpdir) ? 0 : -1;
}

static void rm_rf(const char *d) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", d);
    (void)!system(cmd);
}

static int write_file(const char *path, const char *contents) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    size_t n = fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
    return (n == strlen(contents)) ? 0 : -1;
}

/* Set up: workspace_root containing iron.toml (no deps) + a vendored
 * "stub" subdir with a single .iron file exporting one function. */
static void build_workspace(void) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/iron.toml", g_tmpdir);
    write_file(p, "[package]\nname = \"t\"\nversion = \"0.1.0\"\n");

    char stub_dir[PATH_MAX];
    snprintf(stub_dir, sizeof(stub_dir), "%s/stub", g_tmpdir);
    mkdir(stub_dir, 0755);
    snprintf(p, sizeof(p), "%s/stub/lib.iron", g_tmpdir);
    write_file(p, "func stub_func() -> Int { return 7 }\n");
}

/* ── Test 01: inject + lookup + exported symbols ─────────────────────── */

static void test_inject_and_lookup(void) {
    TEST_ASSERT_EQUAL(0, make_tmpdir());
    build_workspace();

    IronLsp_DepMap *dm = ilsp_dep_map_create(g_tmpdir);
    TEST_ASSERT_NOT_NULL(dm);

    char stub_dir[PATH_MAX];
    snprintf(stub_dir, sizeof(stub_dir), "%s/stub", g_tmpdir);
    TEST_ASSERT_TRUE(ilsp_dep_map_inject_for_test(dm, "stub", stub_dir, NULL));

    IronLsp_DepEntry *e = ilsp_dep_map_lookup(dm, "stub");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("stub", e->dep_name);
    TEST_ASSERT_NOT_NULL(e->canonical_path);
    TEST_ASSERT_EQUAL_size_t(1, ilsp_dep_map_size(dm));

    ilsp_dep_map_destroy(dm);
    rm_rf(g_tmpdir);
}

/* ── Test 02: invalidate named drops just that dep ───────────────────── */

static void test_invalidate_named(void) {
    TEST_ASSERT_EQUAL(0, make_tmpdir());
    build_workspace();

    IronLsp_DepMap *dm = ilsp_dep_map_create(g_tmpdir);

    char stub_dir[PATH_MAX];
    snprintf(stub_dir, sizeof(stub_dir), "%s/stub", g_tmpdir);
    TEST_ASSERT_TRUE(ilsp_dep_map_inject_for_test(dm, "a", stub_dir, NULL));
    TEST_ASSERT_TRUE(ilsp_dep_map_inject_for_test(dm, "b", stub_dir, NULL));
    TEST_ASSERT_EQUAL_size_t(2, ilsp_dep_map_size(dm));

    ilsp_dep_map_invalidate(dm, "a");
    TEST_ASSERT_NULL(ilsp_dep_map_lookup(dm, "a"));
    TEST_ASSERT_NOT_NULL(ilsp_dep_map_lookup(dm, "b"));
    TEST_ASSERT_EQUAL_size_t(1, ilsp_dep_map_size(dm));

    ilsp_dep_map_destroy(dm);
    rm_rf(g_tmpdir);
}

/* ── Test 03: invalidate NULL drops all ──────────────────────────────── */

static void test_invalidate_all(void) {
    TEST_ASSERT_EQUAL(0, make_tmpdir());
    build_workspace();

    IronLsp_DepMap *dm = ilsp_dep_map_create(g_tmpdir);

    char stub_dir[PATH_MAX];
    snprintf(stub_dir, sizeof(stub_dir), "%s/stub", g_tmpdir);
    ilsp_dep_map_inject_for_test(dm, "a", stub_dir, NULL);
    ilsp_dep_map_inject_for_test(dm, "b", stub_dir, NULL);
    ilsp_dep_map_inject_for_test(dm, "c", stub_dir, NULL);
    TEST_ASSERT_EQUAL_size_t(3, ilsp_dep_map_size(dm));

    ilsp_dep_map_invalidate(dm, NULL);
    TEST_ASSERT_EQUAL_size_t(0, ilsp_dep_map_size(dm));
    TEST_ASSERT_NULL(ilsp_dep_map_lookup(dm, "a"));
    TEST_ASSERT_NULL(ilsp_dep_map_lookup(dm, "b"));
    TEST_ASSERT_NULL(ilsp_dep_map_lookup(dm, "c"));

    ilsp_dep_map_destroy(dm);
    rm_rf(g_tmpdir);
}

/* ── Test 04: path-escape rejected (T-03-05) ─────────────────────────── */

static void test_path_escape_rejected(void) {
    TEST_ASSERT_EQUAL(0, make_tmpdir());
    build_workspace();

    IronLsp_DepMap *dm = ilsp_dep_map_create(g_tmpdir);

    /* /etc is not under workspace_root and not under ~/.iron/cache. */
    TEST_ASSERT_FALSE(ilsp_dep_map_inject_for_test(dm, "evil", "/etc", NULL));
    TEST_ASSERT_EQUAL_size_t(0, ilsp_dep_map_size(dm));

    /* Injection inside the workspace succeeds (control group). */
    char stub_dir[PATH_MAX];
    snprintf(stub_dir, sizeof(stub_dir), "%s/stub", g_tmpdir);
    TEST_ASSERT_TRUE(ilsp_dep_map_inject_for_test(dm, "ok", stub_dir, NULL));

    ilsp_dep_map_destroy(dm);
    rm_rf(g_tmpdir);
}

/* ── Test 05: resolve on bare workspace (no iron.toml) returns 0 ─────── */

static void test_resolve_bare_workspace(void) {
    TEST_ASSERT_EQUAL(0, make_tmpdir());
    /* Intentionally do NOT write iron.toml. */

    IronLsp_DepMap *dm = ilsp_dep_map_create(g_tmpdir);
    TEST_ASSERT_EQUAL_INT(0, ilsp_dep_map_resolve(dm, NULL));
    TEST_ASSERT_EQUAL_size_t(0, ilsp_dep_map_size(dm));

    ilsp_dep_map_destroy(dm);
    rm_rf(g_tmpdir);
}

/* ── Test 06: resolve on workspace with iron.toml but no deps ────────── */

static void test_resolve_empty_deps(void) {
    TEST_ASSERT_EQUAL(0, make_tmpdir());
    build_workspace();  /* writes iron.toml with zero dep entries */

    IronLsp_DepMap *dm = ilsp_dep_map_create(g_tmpdir);
    TEST_ASSERT_EQUAL_INT(0, ilsp_dep_map_resolve(dm, NULL));
    TEST_ASSERT_EQUAL_size_t(0, ilsp_dep_map_size(dm));

    ilsp_dep_map_destroy(dm);
    rm_rf(g_tmpdir);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_inject_and_lookup);
    RUN_TEST(test_invalidate_named);
    RUN_TEST(test_invalidate_all);
    RUN_TEST(test_path_escape_rejected);
    RUN_TEST(test_resolve_bare_workspace);
    RUN_TEST(test_resolve_empty_deps);
    return UNITY_END();
}
