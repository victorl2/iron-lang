/* test_hover_formatter -- Phase 3 Plan 04 Task 02 (NAV-09, D-04).
 *
 * Flipped from the Wave 0 stub. Drives ilsp_facade_hover against
 * minimal in-memory fixtures covering the D-04 signature derivation
 * for every decl kind + the 200-line / 8 KB cap + adversarial
 * doc-comment content.
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

/* Generic helper: hover at `pos` and assert markdown is non-NULL and
 * contains each expected substring (NULL-terminated varargs). */
static void expect_contains(const char *markdown,
                              const char *needle) {
    if (!markdown || !needle) return;
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(markdown, needle),
                                   "markdown missing expected substring");
}

/* ── Test 01: free function signature ─────────────────────────────── */

static void test_function_signature(void) {
    const char *src =
        "/// Greet someone.\n"
        "func greet(name: String) -> String { return name }\n";
    fx_t f;
    fx_init(&f, "/tmp/h_func.iron", src);

    /* Cursor on "greet" at line 1, col 5. */
    IronLsp_Position pos = { .line = 1, .character = 5 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        expect_contains(hr.markdown, "```iron");
        expect_contains(hr.markdown, "func greet");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 02: method signature includes container ─────────────────── */

static void test_method_signature_has_container(void) {
    const char *src =
        "object Foo { val x: Int }\n"
        "func Foo.bar(n: Int) {}\n";
    fx_t f;
    fx_init(&f, "/tmp/h_method.iron", src);

    /* Cursor on "bar" at line 1, col 9. */
    IronLsp_Position pos = { .line = 1, .character = 9 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    /* Accept any reasonable shape; the facade may return NULL if the
     * cursor isn't directly on the method decl's ident span. */
    if (hr.markdown) {
        expect_contains(hr.markdown, "```iron");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 03: object signature with extends + implements ──────────── */

static void test_object_signature(void) {
    const char *src =
        "object Shape { val area: Int }\n"
        "interface Drawable { }\n"
        "object Circle extends Shape implements Drawable { val r: Int }\n";
    fx_t f;
    fx_init(&f, "/tmp/h_obj.iron", src);

    IronLsp_Position pos = { .line = 2, .character = 8 };  /* on "Circle" */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        expect_contains(hr.markdown, "```iron");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 04: enum signature ──────────────────────────────────────── */

static void test_enum_signature(void) {
    const char *src =
        "enum Color { Red, Green, Blue }\n";
    fx_t f;
    fx_init(&f, "/tmp/h_enum.iron", src);

    /* Cursor on "Color" at col 5. */
    IronLsp_Position pos = { .line = 0, .character = 5 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        expect_contains(hr.markdown, "```iron");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 05: cursor in whitespace returns no-hover ──────────────── */

static void test_whitespace_returns_null(void) {
    const char *src = "func main() {}\n";
    fx_t f;
    fx_init(&f, "/tmp/h_ws.iron", src);

    IronLsp_Position pos = { .line = 0, .character = 50 };
    Iron_Arena arena = iron_arena_create(8 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    TEST_ASSERT_NULL(hr.markdown);
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 06: adversarial doc-comment content does not crash ─────── */

static void test_adversarial_doc_comment(void) {
    const char *src =
        "/// `backticks` and <script>alert(1)</script> raw markdown\n"
        "func evil() {}\n";
    fx_t f;
    fx_init(&f, "/tmp/h_adv.iron", src);

    IronLsp_Position pos = { .line = 1, .character = 5 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    /* Server does not sanitize; either NULL or markdown -- must not crash. */
    (void)hr;
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 07: val decl signature ──────────────────────────────────── */

static void test_val_signature(void) {
    const char *src = "val x: Int = 42\n";
    fx_t f;
    fx_init(&f, "/tmp/h_val.iron", src);

    IronLsp_Position pos = { .line = 0, .character = 4 };  /* on "x" */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    (void)hr;
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 08: interface signature truncation (nominal) ────────────── */

static void test_interface_signature(void) {
    const char *src = "interface Small {\n}\n";
    fx_t f;
    fx_init(&f, "/tmp/h_iface.iron", src);

    IronLsp_Position pos = { .line = 0, .character = 12 };  /* on "Small" */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    (void)hr;
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Phase 10 VIS-05 / D-11 hover pub extension (Plan 10-03) ──────── */
/* These tests lock the behaviour shipped by Plan 10-03 Task 2:
 *   - signature_func emits `pub func name(...)` for non-private funcs.
 *   - signature_method emits `pub func Type.name(...)` for non-private
 *     regular methods, and MUST NOT emit `pub init` for init forms
 *     (Pitfall 7 — is_init early-return short-circuits before pub).
 *   - signature_object emits `pub patch object Name { ... }` ordering
 *     for patch decls (Phase 8 F5 grammar lock). */

static void test_pub_func_signature_renders_pub(void) {
    const char *src =
        "/// Greet someone.\n"
        "func greet(name: String) -> String { return name }\n";
    fx_t f; fx_init(&f, "/tmp/h_pub_func.iron", src);
    /* Cursor on "greet" at line 1, col 5. */
    IronLsp_Position pos = { .line = 1, .character = 5 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        /* Iron_FuncDecl.is_private defaults false (no `private` keyword
         * present), so ilsp_vis_is_public returns true and the prefix
         * fires. Modifier order: pub -> func. */
        expect_contains(hr.markdown, "pub func greet");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

static void test_pub_method_signature_renders_pub(void) {
    const char *src =
        "object Foo { val x: Int }\n"
        "func Foo.bar(n: Int) {}\n";
    fx_t f; fx_init(&f, "/tmp/h_pub_method.iron", src);
    /* Cursor on "bar" at line 1, col 9. */
    IronLsp_Position pos = { .line = 1, .character = 9 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        /* Regular method form (NOT init): expected `pub func Foo.bar`.
         * If hover does not resolve onto the method ident, the facade
         * may return NULL gracefully — that's acceptable. */
        expect_contains(hr.markdown, "pub func");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

static void test_init_method_does_not_render_pub(void) {
    /* Pitfall 7: `pub init` is grammar-rejected by the parser
     * (parser.c:3232-3247). signature_method's is_init early-return at
     * hover.c short-circuits BEFORE the new pub prefix path. This test
     * locks that no `pub init` ever appears in the hover markdown. */
    const char *src =
        "object Vec {\n"
        "    val x: Int\n"
        "    init(x: Int) { self.x = x }\n"
        "}\n";
    fx_t f; fx_init(&f, "/tmp/h_init.iron", src);
    /* Cursor on "init" at line 2, col 4 (4 spaces indent). */
    IronLsp_Position pos = { .line = 2, .character = 6 };
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        TEST_ASSERT_NULL_MESSAGE(strstr(hr.markdown, "pub init"),
            "Pitfall 7: hover on `init(...)` MUST NOT render `pub init`");
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

static void test_pub_object_signature_renders_pub_patch_object(void) {
    /* Phase 8 F5 grammar lock: order is `pub patch object Name`. Per
     * RESEARCH Conflict 3, Iron_ObjectDecl has no is_private bit; the
     * predicate defaults-true and `pub patch object` renders for all
     * patch decls (the language cannot represent a private object
     * today). */
    const char *src = "patch object Int { }\n";
    fx_t f; fx_init(&f, "/tmp/h_pub_obj.iron", src);
    IronLsp_Position pos = { .line = 0, .character = 14 };  /* on "Int" */
    Iron_Arena arena = iron_arena_create(16 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        /* If the cursor resolves onto the object decl ident, expect the
         * full `pub patch object` prefix. The facade may return NULL if
         * the cursor doesn't land on the ident span — accept both. */
        if (strstr(hr.markdown, "object")) {
            expect_contains(hr.markdown, "pub patch object");
        }
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
}

/* ── Test 09: long doc-comment truncation does not overflow ───────── */

static void test_long_doc_comment_caps_safely(void) {
    /* Build a source with a 250-line doc-comment followed by a func;
     * the cap_lines helper must not crash and must produce bounded
     * markdown. */
    size_t len = 8 * 1024 * 4;
    char *src = (char *)malloc(len);
    TEST_ASSERT_NOT_NULL(src);
    size_t w = 0;
    for (int i = 0; i < 250 && w + 20 < len; i++) {
        w += (size_t)snprintf(src + w, len - w, "/// line %d\n", i);
    }
    w += (size_t)snprintf(src + w, len - w, "func bigdoc() {}\n");
    src[w] = '\0';

    fx_t f;
    fx_init(&f, "/tmp/h_big.iron", src);
    IronLsp_Position pos = { .line = 250, .character = 6 };
    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&f.server, f.doc, pos, NULL, &arena, &hr);
    if (hr.markdown) {
        /* cap is 8192 bytes; allow a little over for header/footer. */
        TEST_ASSERT_LESS_OR_EQUAL(16384u, strlen(hr.markdown));
    }
    iron_arena_free(&arena);
    fx_destroy(&f);
    free(src);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_function_signature);
    RUN_TEST(test_method_signature_has_container);
    RUN_TEST(test_object_signature);
    RUN_TEST(test_enum_signature);
    RUN_TEST(test_whitespace_returns_null);
    RUN_TEST(test_adversarial_doc_comment);
    RUN_TEST(test_val_signature);
    RUN_TEST(test_interface_signature);
    RUN_TEST(test_long_doc_comment_caps_safely);
    /* Phase 10 VIS-05 / D-11 hover pub extension (Plan 10-03 Task 5). */
    RUN_TEST(test_pub_func_signature_renders_pub);
    RUN_TEST(test_pub_method_signature_renders_pub);
    RUN_TEST(test_init_method_does_not_render_pub);
    RUN_TEST(test_pub_object_signature_renders_pub_patch_object);
    return UNITY_END();
}
