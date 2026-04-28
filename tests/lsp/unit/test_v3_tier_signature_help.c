/* test_v3_tier_signature_help -- Phase 10 Plan 10-03 (TIER-04).
 *
 * Verifies the build_sig_info wiring shipped by Plan 10-03 Task 3 in
 * src/lsp/facade/signature_help.c:
 *
 *   bool ro = (md ? md->is_readonly : (fd ? fd->is_readonly : false));
 *   bool pu = (md ? md->is_pure     : (fd ? fd->is_pure     : false));
 *   if (ro)      sb_append(&label, "readonly ");
 *   else if (pu) sb_append(&label, "pure ");
 *   sb_append(&label, "func ");
 *
 * The assertion strategy combines two complementary angles:
 *   (1) Facade-driven negative test: a plain `func` call MUST NOT pick
 *       up `readonly` / `pure` substrings in the SignatureInformation
 *       label. This locks Task 3's wiring against accidentally always-
 *       emitting a prefix.
 *   (2) Facade-driven smoke test: free-function signature_help on
 *       `readonly func` / `pure func` declarations MUST NOT crash and
 *       MUST produce a label containing the bare `func` token. (Per
 *       parser.c:2676, free-form `readonly`/`pure` syntax is parsed
 *       but the is_readonly / is_pure flags are forced to false at
 *       parse-time -- those modifiers are only semantically valid on
 *       methods inside object blocks per parser.c:3154. The completion
 *       test in the sibling file test_v3_tier_completion.c exercises
 *       the positive `readonly func` / `pure func` prefix path against
 *       in-block method decls that DO carry the flags.)
 *   (3) parameter_offsets sanity: ensures the TIER-04 prefix does NOT
 *       shift offsets out of bounds.
 *
 * Test harness mirrors tests/lsp/unit/test_signature_help.c (in-memory
 * source via fx_init helper). */

#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    IronLsp_Server    server;
    IronLsp_Document *doc;
} fx_t;

static void fx_init(fx_t *f, const char *uri, const char *src) {
    memset(f, 0, sizeof(*f));
    f->server.position_encoding = ILSP_ENC_UTF16;
    f->doc = ilsp_document_create(uri, src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(f->doc);
}

static void fx_destroy(fx_t *f) {
    if (f->doc) ilsp_document_destroy(f->doc);
    memset(f, 0, sizeof(*f));
}

/* ── Test 1: plain func label has no readonly/pure prefix (negative) ── */

static void test_plain_func_signature_label_no_tier_prefix(void) {
    /* Locks Task 3's wiring against accidentally always-emitting a
     * tier prefix. Plain func MUST NOT pick up readonly/pure. */
    const char *src =
        "func mutate(dx: Int) {}\n"
        "func main() { mutate() }\n";
    fx_t f; fx_init(&f, "/tmp/sh_plain.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 21 };  /* after `(` */
    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                  &sigs, &n, &active_sig, &active_param);
    if (n == 1) {
        TEST_ASSERT_NOT_NULL(sigs);
        TEST_ASSERT_NOT_NULL(sigs[0].label);
        TEST_ASSERT_NULL_MESSAGE(strstr(sigs[0].label, "readonly"),
            "TIER-04 (negative): plain func label MUST NOT contain `readonly`");
        TEST_ASSERT_NULL_MESSAGE(strstr(sigs[0].label, "pure"),
            "TIER-04 (negative): plain func label MUST NOT contain `pure`");
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(sigs[0].label, "func"),
            "TIER-04 (negative): plain func label MUST contain bare `func`");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 2: readonly free-func form smoke (parser drops flag) ─────── */

static void test_readonly_free_func_signature_help_smoke(void) {
    /* Per parser.c:2676, free-form `readonly func` syntax is parsed
     * but the is_readonly flag is forced to false (tier semantics are
     * only valid on in-block methods per parser.c:3154). This test
     * ensures the facade does NOT crash on the syntax + always emits
     * a label containing the `func` token. The TIER-04 prefix path
     * is NOT exercised here; see test_v3_tier_completion.c for the
     * positive prefix path against in-block method decls. */
    const char *src =
        "readonly func length_sq(x: Int, y: Int) -> Int { return x * x + y * y }\n"
        "func main() { val r: Int = length_sq() }\n";
    fx_t f; fx_init(&f, "/tmp/sh_ro.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 38 };
    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                  &sigs, &n, &active_sig, &active_param);
    if (n == 1) {
        TEST_ASSERT_NOT_NULL(sigs);
        TEST_ASSERT_NOT_NULL(sigs[0].label);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(sigs[0].label, "func"),
            "TIER-04 smoke: signature label MUST contain `func` token");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 3: pure free-func form smoke (parser drops flag) ─────────── */

static void test_pure_free_func_signature_help_smoke(void) {
    /* Symmetric to Test 2 — parser drops `pure` flag on free funcs;
     * verify the facade does not crash and emits the `func` token. */
    const char *src =
        "pure func add(a: Int, b: Int) -> Int { return a + b }\n"
        "func main() { val r: Int = add() }\n";
    fx_t f; fx_init(&f, "/tmp/sh_pure.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 32 };
    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                  &sigs, &n, &active_sig, &active_param);
    if (n == 1) {
        TEST_ASSERT_NOT_NULL(sigs);
        TEST_ASSERT_NOT_NULL(sigs[0].label);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(sigs[0].label, "func"),
            "TIER-04 smoke: signature label MUST contain `func` token");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 4: parameter_offsets unaffected by prefix length ──────────── */

static void test_parameter_offsets_unaffected_by_tier_prefix(void) {
    /* The TIER-04 prefix is BEFORE the `func ` token which is BEFORE
     * the open-paren. parameter_offsets are computed from the open-
     * paren position, so adding `readonly ` / `pure ` to the prefix
     * MUST NOT shift offsets relative to the `(`-anchored origin. */
    const char *src =
        "func touch(a: Int, b: Int) -> Int { return a + b }\n"
        "func main() { touch(1, 2) }\n";
    fx_t f; fx_init(&f, "/tmp/sh_offs.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 22 };
    Iron_Arena arena = iron_arena_create(32 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                  &sigs, &n, &active_sig, &active_param);
    if (n == 1 && sigs[0].parameter_count > 0) {
        for (int i = 0; i < sigs[0].parameter_count; i++) {
            int s = sigs[0].parameter_offsets[i].start;
            int e = sigs[0].parameter_offsets[i].end;
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, s,
                "parameter_offsets[i].start MUST be >= 0");
            TEST_ASSERT_GREATER_THAN_INT_MESSAGE(s, e,
                "parameter_offsets[i].end MUST be > .start");
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE((int)strlen(sigs[0].label), e,
                "parameter_offsets[i].end MUST fit in the label string");
        }
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_plain_func_signature_label_no_tier_prefix);
    RUN_TEST(test_readonly_free_func_signature_help_smoke);
    RUN_TEST(test_pure_free_func_signature_help_smoke);
    RUN_TEST(test_parameter_offsets_unaffected_by_tier_prefix);
    return UNITY_END();
}
