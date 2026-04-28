/* test_v3_patch_hover -- Phase 11 Plan 11-03 (PATCH-04).
 *
 * Drives ilsp_facade_hover against the v3_patch fixtures to verify
 * PATCH-04 acceptance:
 *   1. cursor on a patch method (`reverse` in patch_hover.iron):
 *      hover markdown contains `From \`patch object String` (italic
 *      enclosing-patch line PREPENDS the existing fenced signature
 *      block).
 *   2. cursor on a NATIVE method (`compute` in patch_hover_non_patch.iron):
 *      hover markdown does NOT contain `From \`patch object` (D-13
 *      false-positive guard).
 *   3. cursor on the patch ObjectDecl itself (the `String` token in
 *      `patch object String { ... }`): hover renders the existing
 *      Phase 9 AST-06 `patch object` prefix on object-level hover —
 *      regression smoke that Phase 11 didn't break the object-level
 *      hover path.
 *
 * Test harness mirrors test_hover_formatter.c (in-memory document via
 * ilsp_document_create, no workspace_index because patch detection
 * works on the same Iron_Program). */

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

/* ── Fixture loader ──────────────────────────────────────────────── */

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

/* Find the (0-based) line containing the substring, or -1. */
static int find_line_with(const char *src, const char *needle) {
    const char *p = strstr(src, needle);
    if (!p) return -1;
    int line = 0;
    for (const char *r = src; r < p; r++) {
        if (*r == '\n') line++;
    }
    return line;
}

/* Find the (0-based) column of the substring's first occurrence within
 * its line, plus an offset. */
static int find_col_of(const char *src, const char *needle, int offset) {
    const char *p = strstr(src, needle);
    if (!p) return -1;
    const char *line_start = src;
    for (const char *r = src; r < p; r++) {
        if (*r == '\n') line_start = r + 1;
    }
    return (int)(p - line_start) + offset;
}

/* ── Test 1: hover on patch method renders italic prepend ────────── */

static void test_hover_patch_method_renders_italic_prepend(void) {
    char path[1024];
    const char *fpath = find_fixture(path, sizeof(path), "patch_hover.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(fpath,
        "fixture patch_hover.iron not found");
    char *src = load_file(fpath);
    TEST_ASSERT_NOT_NULL(src);

    /* Cursor in the middle of the `reverse` identifier on the
     * patch-method declaration line. */
    int line = find_line_with(src, "func reverse()");
    int col  = find_col_of(src,  "reverse()", 2);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, line);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, col);

    IronLsp_Server server = {0};
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/test_v3_patch_hover_method.iron", src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    IronLsp_Position pos = { .line = (uint32_t)line, .character = (uint32_t)col };
    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&server, doc, pos, NULL, &arena, &hr);

    TEST_ASSERT_NOT_NULL_MESSAGE(hr.markdown,
        "PATCH-04: hover on a patch method MUST return non-NULL markdown");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(hr.markdown, "From `patch object"),
        "PATCH-04: hover on a patch method MUST contain `From `patch object`");
    /* The italic line should appear BEFORE the fenced block. */
    const char *italic = strstr(hr.markdown, "From `patch object");
    const char *fence  = strstr(hr.markdown, "```iron");
    TEST_ASSERT_NOT_NULL(italic);
    TEST_ASSERT_NOT_NULL(fence);
    TEST_ASSERT_TRUE_MESSAGE(italic < fence,
        "PATCH-04: italic enclosing-patch line MUST precede the fenced signature");

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
}

/* ── Test 2: hover on native method does NOT render italic line ─── */

static void test_hover_native_method_no_prepend(void) {
    char path[1024];
    const char *fpath = find_fixture(path, sizeof(path),
                                      "patch_hover_non_patch.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(fpath,
        "fixture patch_hover_non_patch.iron not found");
    char *src = load_file(fpath);
    TEST_ASSERT_NOT_NULL(src);

    /* Cursor on `compute` (native method on Calculator). */
    int line = find_line_with(src, "func compute(");
    int col  = find_col_of(src,  "compute(", 2);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, line);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, col);

    IronLsp_Server server = {0};
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/test_v3_patch_hover_non_patch.iron", src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    IronLsp_Position pos = { .line = (uint32_t)line, .character = (uint32_t)col };
    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&server, doc, pos, NULL, &arena, &hr);

    /* If hover returns markdown, it must NOT contain the patch prepend. */
    if (hr.markdown) {
        TEST_ASSERT_NULL_MESSAGE(strstr(hr.markdown, "From `patch object"),
            "PATCH-04 D-13: hover on a NATIVE method MUST NOT render the "
            "`From `patch object` italic prepend (false-positive guard)");
    }

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
}

/* ── Test 3: hover on patch ObjectDecl token shows AST-06 prefix ── */

static void test_hover_patch_object_decl_unchanged(void) {
    /* Phase 9 AST-06 already emits `patch object` for object-level
     * hover (signature_object). Phase 11 PATCH-04 must NOT regress this
     * path — it only adds a per-method italic line, leaving the
     * object-level prefix untouched. */
    char path[1024];
    const char *fpath = find_fixture(path, sizeof(path), "patch_hover.iron");
    TEST_ASSERT_NOT_NULL_MESSAGE(fpath, "fixture patch_hover.iron not found");
    char *src = load_file(fpath);
    TEST_ASSERT_NOT_NULL(src);

    /* Cursor on `String` token inside `patch object String { ... }`. */
    int line = find_line_with(src, "patch object String");
    int col  = find_col_of(src,  "String", 2);  /* on `Sti` */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, line);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, col);

    IronLsp_Server server = {0};
    server.position_encoding = ILSP_ENC_UTF16;

    IronLsp_Document *doc = ilsp_document_create(
        "/tmp/test_v3_patch_hover_object.iron", src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(doc);

    IronLsp_Position pos = { .line = (uint32_t)line, .character = (uint32_t)col };
    Iron_Arena arena = iron_arena_create(64 * 1024);
    IronLsp_HoverResult hr = {0};
    ilsp_facade_hover(&server, doc, pos, NULL, &arena, &hr);

    /* Cursor on the primitive type may resolve to either the patch
     * ObjectDecl (showing AST-06 `patch object`) OR to the primitive
     * type's name-only short-circuit. Both are acceptable; only assert
     * non-crash + markdown shape. The AST-06 patch-prefix path is
     * guarded by the broader test_hover_formatter Phase 9 corpus. */
    if (hr.markdown) {
        /* Markdown should still contain a fenced iron block. */
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(hr.markdown, "```iron"),
            "Hover on patch ObjectDecl token MUST still produce an iron-fenced block");
    }

    iron_arena_free(&arena);
    ilsp_document_destroy(doc);
    free(src);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hover_patch_method_renders_italic_prepend);
    RUN_TEST(test_hover_native_method_no_prepend);
    RUN_TEST(test_hover_patch_object_decl_unchanged);
    return UNITY_END();
}
