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
                                                &h->arena, &h->diags, NULL);
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
    TEST_IGNORE_MESSAGE("Phase 9 Plan 01 Task 2 implementation pending");
}

/* ── Test 02: named-init body cursor resolves correctly ─────────────── */
static void test_named_init_resolves(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 01 Task 2 implementation pending");
}

/* ── Test 03: patch-method body cursor resolves correctly ───────────── */
static void test_patch_method_body_resolves(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 01 Task 2 implementation pending");
}

/* Suppress -Wunused-function for harness helpers in Wave 0. They become
 * live in Task 2 when TEST_IGNORE is removed. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void wave0_helpers_alive(void) {
    char buf[16];
    (void)fixture_path(buf, sizeof(buf), "x.iron");
    V3NavHarness h;
    (void)h;
    (void)harness_init_from_file;
    (void)harness_free;
    (void)load_file;
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_init_body_resolves);
    RUN_TEST(test_named_init_resolves);
    RUN_TEST(test_patch_method_body_resolves);
    return UNITY_END();
}
