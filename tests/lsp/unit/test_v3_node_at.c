/* test_v3_node_at -- Phase 9 Plan 01 (Wave 0 Task 1 / flipped in Task 2).
 *
 * D-12 cases 1-3 verified by fixture:
 *   1. cursor inside `init(args) { ... }` body resolves to METHOD_DECL
 *      with is_init=true && init_name==NULL.
 *   2. cursor inside `init zero() { ... }` body resolves to METHOD_DECL
 *      with is_init=true && strcmp(init_name,"zero")==0.
 *   3. cursor inside `patch object Int { func double() { ... } }` body
 *      resolves to METHOD_DECL whose owning Iron_ObjectDecl has
 *      is_patch=true && target_type_name=="Int".
 *
 * Wave 0: TEST_IGNORE'd stubs to register tests under
 * phase-m2-invariant. Task 2 flips them to real assertions after D-06
 * walker descent is fixture-verified.
 */
#include "unity.h"

#include "lsp/facade/nav/node_at.h"

#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Fixture loader ──────────────────────────────────────────────────── */

typedef struct {
    IronLsp_Document *doc;
    Iron_Arena        arena;
    Iron_DiagList     diags;
    Iron_Program     *program;
    char             *source;     /* heap-allocated; freed in harness_free */
} V3NavHarness;

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

static bool harness_init_from_file(V3NavHarness *h, const char *path,
                                     const char *uri) {
    memset(h, 0, sizeof(*h));
    h->source = load_file(path);
    if (!h->source) return false;
    h->arena = iron_arena_create(64 * 1024);
    h->diags = iron_diaglist_create();
    h->doc   = ilsp_document_create(uri, h->source, strlen(h->source), 1);
    Iron_AnalyzeResult r = iron_analyze_buffer(h->source, strlen(h->source),
                                                uri, IRON_ANALYSIS_MODE_LSP,
                                                &h->arena, &h->diags, NULL,
        0);
    h->program = r.program;
    return h->program != NULL;
}

static void harness_free(V3NavHarness *h) {
    if (h->doc) ilsp_document_destroy(h->doc);
    iron_diaglist_free(&h->diags);
    iron_arena_free(&h->arena);
    free(h->source);
    memset(h, 0, sizeof(*h));
}

/* Locate fixture path: tests run from build/, fixtures live at
 * ../tests/integration/. Provide a portable resolver. */
static const char *fixture_path(char *buf, size_t cap, const char *name) {
    snprintf(buf, cap, "../tests/integration/%s", name);
    FILE *f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#ifdef IRON_SOURCE_TREE_ROOT
    snprintf(buf, cap, "%s/tests/integration/%s",
             IRON_SOURCE_TREE_ROOT, name);
    f = fopen(buf, "rb");
    if (f) { fclose(f); return buf; }
#endif
    return NULL;
}

/* ── Test 01: init body cursor resolves to METHOD_DECL (anonymous) ──── */
static void test_init_body_resolves(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf),
                                     "v3_init_anonymous_and_named.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "fixture v3_init_anonymous_and_named.iron not found");
    V3NavHarness h;
    bool ok = harness_init_from_file(&h, path, path);
    TEST_ASSERT_TRUE_MESSAGE(ok, "analyze of init fixture failed");

    /* Source line 13 (1-based) is "        self.count = v" — the body
     * of the anonymous init(v: Int) { ... }. LSP Position is 0-based,
     * so line 12, column 14 lands inside "count". */
    IronLsp_Position pos = { .line = 12, .character = 14 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL_MESSAGE(n, "node_at returned NULL inside init body");
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_NODE_METHOD_DECL, n->kind,
        "cursor inside init body should resolve to METHOD_DECL");
    Iron_MethodDecl *md = (Iron_MethodDecl *)n;
    TEST_ASSERT_TRUE_MESSAGE(md->is_init,
        "anonymous init MUST have is_init=true");
    TEST_ASSERT_NULL_MESSAGE(md->init_name,
        "anonymous init MUST have init_name==NULL");
    TEST_ASSERT_NOT_NULL(md->type_name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Counter", md->type_name,
        "init's owning type_name MUST match source object");

    harness_free(&h);
}

/* ── Test 02: named-init body cursor resolves correctly ─────────────── */
static void test_named_init_resolves(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf),
                                     "v3_init_anonymous_and_named.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "fixture v3_init_anonymous_and_named.iron not found");
    V3NavHarness h;
    bool ok = harness_init_from_file(&h, path, path);
    TEST_ASSERT_TRUE_MESSAGE(ok, "analyze of init fixture failed");

    /* Source line 17 (1-based) "        self.count = 0" — body of
     * `init zero() { ... }`. Position is 0-based: line 16 col 14. */
    IronLsp_Position pos = { .line = 16, .character = 14 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL_MESSAGE(n, "node_at returned NULL inside named-init body");
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_NODE_METHOD_DECL, n->kind,
        "cursor inside named-init body should resolve to METHOD_DECL");
    Iron_MethodDecl *md = (Iron_MethodDecl *)n;
    TEST_ASSERT_TRUE_MESSAGE(md->is_init,
        "named init MUST have is_init=true");
    TEST_ASSERT_NOT_NULL_MESSAGE(md->init_name,
        "named init MUST have non-NULL init_name");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("zero", md->init_name,
        "init_name MUST match source token");
    TEST_ASSERT_NOT_NULL(md->type_name);
    TEST_ASSERT_EQUAL_STRING("Counter", md->type_name);

    harness_free(&h);
}

/* ── Test 03: patch-method body cursor resolves correctly ───────────── */
static void test_patch_method_body_resolves(void) {
    char buf[1024];
    const char *path = fixture_path(buf, sizeof(buf),
                                     "v3_patch_primitive.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(path, "fixture v3_patch_primitive.iron not found");
    V3NavHarness h;
    bool ok = harness_init_from_file(&h, path, path);
    TEST_ASSERT_TRUE_MESSAGE(ok, "analyze of patch fixture failed");

    /* Source line 3 (1-based) "        return self * 2" — body of
     * `pub readonly func double() -> Int`. Position is 0-based: line 2
     * col 14. */
    IronLsp_Position pos = { .line = 2, .character = 14 };
    Iron_Node *n = ilsp_nav_node_at(h.doc, h.program, pos, ILSP_ENC_UTF16);
    TEST_ASSERT_NOT_NULL_MESSAGE(n, "node_at returned NULL inside patch-method body");
    TEST_ASSERT_EQUAL_INT_MESSAGE(IRON_NODE_METHOD_DECL, n->kind,
        "cursor inside patch-method body should resolve to METHOD_DECL");
    Iron_MethodDecl *md = (Iron_MethodDecl *)n;
    TEST_ASSERT_NOT_NULL(md->type_name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Int", md->type_name,
        "patch-method type_name MUST match the patch target");

    /* Walk program->decls[] for the owning Iron_ObjectDecl with
     * is_patch=true && target_type_name=="Int". */
    bool found_patch_object = false;
    for (int i = 0; i < h.program->decl_count; i++) {
        Iron_Node *d = h.program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (od->is_patch && od->target_type_name &&
            strcmp(od->target_type_name, "Int") == 0) {
            found_patch_object = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_patch_object,
        "expected an Iron_ObjectDecl with is_patch=true && target=='Int'");

    harness_free(&h);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_body_resolves);
    RUN_TEST(test_named_init_resolves);
    RUN_TEST(test_patch_method_body_resolves);
    return UNITY_END();
}
