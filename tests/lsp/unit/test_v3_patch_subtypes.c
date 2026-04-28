/* test_v3_patch_subtypes -- Phase 11 Plan 11-02 (PATCH-02).
 *
 * Drives ilsp_facade_nav_type_hierarchy_subtypes against the v3_patch
 * fixtures to verify PATCH-02 acceptance:
 *   1. typeHierarchy/subtypes on a user object T returns
 *      TypeHierarchyItem[] containing patch methods as virtual
 *      kind=SymbolKind.Method (6) entries with detail starting
 *      "[patch from ".
 *   2. Native subtype entries continue to have detail == NULL
 *      (Conflict 2 forward-compat — JSON serializer omits the
 *      field when NULL).
 *   3. struct-extension drift-guard: IronLsp_TypeHierarchyItem
 *      compiles with a `detail` member at the source level (this
 *      file references it directly; if the field were absent,
 *      compilation would fail).
 *
 * Test harness mirrors test_type_hierarchy.c: builds a tmp workspace
 * directory via mkdtemp + writes the fixture, instantiates a real
 * IronLsp_WorkspaceIndex with warm-seed, and queries subtypes through
 * the facade entry. The patch-emit walk in type_hierarchy.c::
 * th_patch_emit_visit handles the patch identification.
 */

#include "unity.h"

#include "lsp/facade/nav/type_hierarchy.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/facade/types.h"
#include "util/arena.h"

#include <dirent.h>
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

/* ── Fixture-path helper ─────────────────────────────────────────── */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static const char *find_fixture(char *out, size_t cap, const char *name) {
#ifdef IRON_SOURCE_TREE_ROOT
    snprintf(out, cap, "%s/tests/lsp/unit/v3_patch/%s",
             IRON_SOURCE_TREE_ROOT, name);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return out; }
#endif
    snprintf(out, cap, "../tests/lsp/unit/v3_patch/%s", name);
    FILE *f2 = fopen(out, "rb");
    if (f2) { fclose(f2); return out; }
    return NULL;
}

/* ── Tmp-workspace harness (mirrors make_extender_fixture pattern) ── */

static char *make_workspace_with_fixture(const char *fixture_name,
                                           const char *target_filename) {
    char tmpl[] = "/tmp/iron_v3_patch_subtypes_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return NULL;
    char *out = strdup(dir);

    char fixture_buf[1024];
    const char *fpath = find_fixture(fixture_buf, sizeof(fixture_buf),
                                       fixture_name);
    if (!fpath) { free(out); return NULL; }
    char *src = load_file(fpath);
    if (!src) { free(out); return NULL; }

    char target[512];
    snprintf(target, sizeof(target), "%s/%s", out, target_filename);
    FILE *fp = fopen(target, "w");
    if (!fp) { free(src); free(out); return NULL; }
    fputs(src, fp);
    fclose(fp);
    free(src);
    return out;
}

static void cleanup_workspace(char *dir) {
    if (!dir) return;
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
            unlink(path);
        }
        closedir(dp);
    }
    rmdir(dir);
    free(dir);
}

/* ── Test 1: subtypes on user object surfaces patch methods ─────── */

static void test_subtypes_user_object_surfaces_patch_method(void) {
    char *dir = make_workspace_with_fixture(
        "patch_subtypes_user_object.iron", "fx.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(dir, "tmp workspace setup failed");

    IronLsp_WorkspaceIndex *wi = ilsp_workspace_index_create(dir);
    TEST_ASSERT_NOT_NULL(wi);
    ilsp_workspace_index_warm_seed(wi, NULL);

    IronLsp_Server srv = {0};
    srv.position_encoding = ILSP_ENC_UTF16;
    srv.workspace_index   = wi;

    char path[512];
    snprintf(path, sizeof(path), "%s/fx.iron", dir);

    /* Build triple keyed on Circle (the user object that has both
     * a patch and is a candidate for subtypes-of-Circle). */
    Iron_Arena tmp = iron_arena_create(8 * 1024);
    Iron_Symbol fake = {0};
    fake.name = "Circle";
    fake.sym_kind = IRON_SYM_TYPE;
    IronLsp_SymbolId triple = ilsp_symbol_id_derive(&fake, path, NULL, &tmp);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_TypeHierarchyItem *items = NULL;
    size_t n = 0;
    ilsp_facade_nav_type_hierarchy_subtypes(&srv, triple, NULL,
                                               &arena, &items, &n);

    /* Expect at least one entry — the patch method `area_doubled`. */
    TEST_ASSERT_GREATER_OR_EQUAL_size_t_MESSAGE(1, n,
        "PATCH-02: subtypes on Circle MUST surface at least one patch "
        "method (`area_doubled`) — got 0 items");

    /* At least one item should be a patch entry: kind=Method (6) and
     * detail starts with "[patch from ". */
    bool saw_patch = false;
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind == 6 && items[i].detail &&
            strncmp(items[i].detail, "[patch from ", 12) == 0) {
            saw_patch = true;
            /* Also verify the patch name made it through. */
            TEST_ASSERT_NOT_NULL(items[i].name);
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(saw_patch,
        "PATCH-02: subtypes MUST surface at least one entry with "
        "kind=SymbolKind.Method (6) AND detail starting `[patch from `");

    iron_arena_free(&arena);
    iron_arena_free(&tmp);
    ilsp_workspace_index_destroy(wi);
    cleanup_workspace(dir);
}

/* ── Test 2: struct extension drift-guard ────────────────────────── */

/* This compile-time test exists primarily to lock in the existence of
 * the `detail` field on IronLsp_TypeHierarchyItem (Conflict 2). If the
 * struct ever loses the field, this file will fail to compile.
 *
 * The runtime check confirms zero-init via {0} sets detail to NULL,
 * matching the convention native subtypes rely on (Conflict 2 forward-
 * compat — JSON serializer omits the field when NULL). */
static void test_typehierarchyitem_detail_field_default_null(void) {
    IronLsp_TypeHierarchyItem zero = {0};
    TEST_ASSERT_NULL_MESSAGE(zero.detail,
        "Conflict 2 drift-guard: zero-init IronLsp_TypeHierarchyItem MUST "
        "have detail == NULL (native-subtype default)");
    /* Struct field also must be writable as expected. */
    zero.detail = "[patch from somewhere]";
    TEST_ASSERT_NOT_NULL(zero.detail);
}

/* ── Test 3: native subtypes carry NULL detail (Conflict 2) ─────── */

static void test_native_subtypes_have_null_detail(void) {
    /* Build a workspace with two files: parent + child. Subtypes-of-parent
     * returns the child as a kind=Class entry; that entry's detail field
     * MUST be NULL (no patch contribution). */
    char tmpl[] = "/tmp/iron_v3_patch_native_detail_XXXXXX";
    char *dir = mkdtemp(tmpl);
    TEST_ASSERT_NOT_NULL(dir);

    char path0[512];
    snprintf(path0, sizeof(path0), "%s/parent.iron", dir);
    FILE *fp0 = fopen(path0, "w");
    TEST_ASSERT_NOT_NULL(fp0);
    fputs("object Parent { val x: Int }\n"
          "object Child extends Parent { val y: Int }\n", fp0);
    fclose(fp0);

    IronLsp_WorkspaceIndex *wi = ilsp_workspace_index_create(dir);
    TEST_ASSERT_NOT_NULL(wi);
    ilsp_workspace_index_warm_seed(wi, NULL);

    IronLsp_Server srv = {0};
    srv.position_encoding = ILSP_ENC_UTF16;
    srv.workspace_index   = wi;

    Iron_Arena tmp = iron_arena_create(8 * 1024);
    Iron_Symbol fake = {0};
    fake.name = "Parent";
    fake.sym_kind = IRON_SYM_TYPE;
    IronLsp_SymbolId triple = ilsp_symbol_id_derive(&fake, path0, NULL, &tmp);

    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_TypeHierarchyItem *items = NULL;
    size_t n = 0;
    ilsp_facade_nav_type_hierarchy_subtypes(&srv, triple, NULL,
                                               &arena, &items, &n);

    /* Per-item drift-guard: any non-patch entry (kind != 6) MUST have
     * detail == NULL so the JSON serializer omits the field on the
     * wire. Patch entries (kind == 6) carry a non-NULL detail. */
    for (size_t i = 0; i < n; i++) {
        if (items[i].kind == 6) {
            TEST_ASSERT_NOT_NULL_MESSAGE(items[i].detail,
                "patch entry MUST have non-NULL detail");
        } else {
            TEST_ASSERT_NULL_MESSAGE(items[i].detail,
                "Conflict 2 drift-guard: native subtype entry MUST have "
                "detail == NULL so JSON serializer omits the field");
        }
    }

    iron_arena_free(&arena);
    iron_arena_free(&tmp);
    ilsp_workspace_index_destroy(wi);

    /* cleanup — `dir` is the stack-allocated tmpl[] buffer (mkdtemp
     * mutates it in place and returns the same pointer); do NOT free. */
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.') continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
            unlink(p);
        }
        closedir(dp);
    }
    rmdir(dir);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_subtypes_user_object_surfaces_patch_method);
    RUN_TEST(test_typehierarchyitem_detail_field_default_null);
    RUN_TEST(test_native_subtypes_have_null_detail);
    return UNITY_END();
}
