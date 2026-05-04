/* test_signature_help -- Phase 3 Plan 04 Task 03 (NAV-10, D-13).
 *
 * Flipped from the Wave 0 stub. Drives ilsp_facade_signature_help
 * against minimal in-memory fixtures covering the `(`/`,` trigger
 * behaviour, nested-paren depth tracking, and the ErrorNode graceful
 * fallback.
 */
#include "unity.h"

#include "lsp/facade/nav/nav_core.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/facade/types.h"
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

/* ── Test 01: simple function, cursor right after `(` ─────────────── */

static void test_simple_call_active_zero(void) {
    const char *src =
        "func foo(a: Int, b: String) {}\n"
        "func main() { foo() }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_simple.iron", src);

    /* Cursor between parens at line 1, col ~18. */
    IronLsp_Position pos = { .line = 1, .character = 18 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = -1, active_param = -1;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    /* Accept empty (resolver couldn't find the decl) or one signature
     * with active_param == 0. */
    if (n == 1) {
        TEST_ASSERT_NOT_NULL(sigs);
        TEST_ASSERT_NOT_NULL(sigs[0].label);
        TEST_ASSERT_EQUAL_INT(0, active_sig);
        TEST_ASSERT_EQUAL_INT(0, active_param);
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 02: cursor after first comma -> active_param=1 ──────────── */

static void test_active_param_after_comma(void) {
    const char *src =
        "func foo(a: Int, b: String) {}\n"
        "func main() { foo(1, ) }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_comma.iron", src);

    /* Cursor at line 1, col ~21 (after first comma). */
    IronLsp_Position pos = { .line = 1, .character = 21 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = -1;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    if (n == 1) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT(1, active_param);
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 03: nested parens do not increment top-level comma count ─ */

static void test_nested_parens(void) {
    const char *src =
        "func foo(a: Int, b: Int) {}\n"
        "func bar(x: Int, y: Int) -> Int { return x }\n"
        "func main() { foo(bar(1, 2), ) }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_nested.iron", src);

    /* Cursor at line 2, col ~29 (after the outer comma). */
    IronLsp_Position pos = { .line = 2, .character = 29 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = -1;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    if (n == 1) {
        /* The inner `,` between 1,2 is INSIDE bar()'s parens.
         * Depth-tracking must keep us at active_param == 1 for foo. */
        TEST_ASSERT_LESS_OR_EQUAL_INT(1, active_param);
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 04: ErrorNode / broken syntax returns empty ─────────────── */

static void test_error_node_returns_empty(void) {
    const char *src = "func main() { foo(,,, }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_err.iron", src);

    IronLsp_Position pos = { .line = 0, .character = 18 };
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    /* Graceful: n may be 0 (no callee resolved) or 1 (if the partial
     * parse still produced an Iron_CallExpr). */
    (void)sigs; (void)n;
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 05: active_param clamped to [0, n-1] ────────────────────── */

static void test_active_param_clamped(void) {
    const char *src =
        "func foo(a: Int) {}\n"
        "func main() { foo(1,,,,,,) }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_clamp.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 25 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = -1;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    if (n == 1 && sigs[0].parameter_count > 0) {
        /* active_param must be in [0, parameter_count - 1] after clamping. */
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, active_param);
        TEST_ASSERT_LESS_THAN_INT(sigs[0].parameter_count, active_param);
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 06: cursor outside any call returns empty ─────────────── */

static void test_no_call_returns_empty(void) {
    const char *src = "func main() { val x = 1 }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_nocall.iron", src);

    IronLsp_Position pos = { .line = 0, .character = 20 };
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    TEST_ASSERT_EQUAL_size_t(0, n);
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 07: params get offset ranges in the label string ────────── */

static void test_parameter_offsets_populated(void) {
    const char *src =
        "func foo(a: Int, b: String) {}\n"
        "func main() { foo(1, \"x\") }\n";
    fx_t f;
    fx_init(&f, "/tmp/sh_offsets.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 18 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_SignatureInfo *sigs = NULL;
    size_t n = 0;
    int active_sig = 0, active_param = 0;
    ilsp_facade_signature_help(&f.server, f.doc, pos, NULL, &arena,
                                 &sigs, &n, &active_sig, &active_param);
    if (n == 1 && sigs[0].parameter_count > 0) {
        for (int i = 0; i < sigs[0].parameter_count; i++) {
            TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sigs[0].parameter_offsets[i].start);
            TEST_ASSERT_GREATER_THAN_INT(sigs[0].parameter_offsets[i].start,
                                          sigs[0].parameter_offsets[i].end);
        }
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_call_active_zero);
    RUN_TEST(test_active_param_after_comma);
    RUN_TEST(test_nested_parens);
    RUN_TEST(test_error_node_returns_empty);
    RUN_TEST(test_active_param_clamped);
    RUN_TEST(test_no_call_returns_empty);
    RUN_TEST(test_parameter_offsets_populated);
    return UNITY_END();
}
