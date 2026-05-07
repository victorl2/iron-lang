/* Phase 17 Wave 0: TDD scaffold for VAL-03 (post-init val-field write) +
 * VAL-04 (var field reassignability).
 *
 * Authored RED first; flips GREEN once Plan 17-02 lands
 * IRON_ERR_VAL_FIELD_REASSIGN=265 in src/diagnostics/diagnostics.h and
 * extends the existing self-val-write check at typecheck.c:3924-3969 with
 * a non-pub val sibling branch.
 *
 * Driver runs the FULL analyzer pipeline (iron_analyze_buffer) so the test
 * is implementation-site-agnostic — wherever the diagnostic surfaces
 * within the analyzer pass list, the assertions catch it. */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <stdatomic.h>
#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Lex + parse + analyze via the production buffer entry point. */
static void analyze_src(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    (void)r;
}

static int count_with_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}
static const char *first_msg_with_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].message;
    }
    return NULL;
}
static const char *first_sug_with_code(int target) {
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) return diags.items[i].suggestion;
    }
    return NULL;
}

void test_val_03_post_init_val_field_write_rejected(void) {
    analyze_src(
        "object T {\n"
        "    val x: Int\n"
        "    init() { self.x = 1 }\n"
        "    func bad() { self.x = 2 }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_VAL_FIELD_REASSIGN));
}

void test_val_03_message_substring(void) {
    analyze_src(
        "object T {\n"
        "    val x: Int\n"
        "    init() { self.x = 1 }\n"
        "    func bad() { self.x = 2 }\n"
        "}\n");
    const char *m = first_msg_with_code(IRON_ERR_VAL_FIELD_REASSIGN);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NOT_NULL(strstr(m, "cannot reassign"));
}

void test_val_03_hint_present(void) {
    analyze_src(
        "object T {\n"
        "    val x: Int\n"
        "    init() { self.x = 1 }\n"
        "    func bad() { self.x = 2 }\n"
        "}\n");
    const char *s = first_sug_with_code(IRON_ERR_VAL_FIELD_REASSIGN);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(strstr(s, "declare field as 'var'"));
}

void test_val_03_init_body_val_field_write_permitted(void) {
    analyze_src(
        "object T {\n"
        "    val x: Int\n"
        "    init() { self.x = 1 }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_FIELD_REASSIGN));
}

void test_val_03_pub_val_uses_existing_code(void) {
    /* Pub-val path at typecheck.c:4011-4023 fires IRON_ERR_VAL_REASSIGN=203
     * — NOT the new code 265. Distinct routing per CONTEXT.md decision. The
     * new branch is guarded by !mf->is_pub so it cannot fire on pub fields. */
    analyze_src(
        "object T {\n"
        "    pub val x: Int\n"
        "    init() { self.x = 1 }\n"
        "    func bad() { self.x = 2 }\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_VAL_REASSIGN));
}

void test_val_04_var_field_reassign_permitted(void) {
    analyze_src(
        "object T {\n"
        "    var x: Int\n"
        "    init() { self.x = 1 }\n"
        "    func ok() { self.x = 2 }\n"
        "}\n");
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_REASSIGN));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_FIELD_REASSIGN));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_val_03_post_init_val_field_write_rejected);
    RUN_TEST(test_val_03_message_substring);
    RUN_TEST(test_val_03_hint_present);
    RUN_TEST(test_val_03_init_body_val_field_write_permitted);
    RUN_TEST(test_val_03_pub_val_uses_existing_code);
    RUN_TEST(test_val_04_var_field_reassign_permitted);
    return UNITY_END();
}
