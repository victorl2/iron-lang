/* Phase 9 Plan 09-02 — v3 printer fixed-point parity test.
 *
 * AST-05 deliverable: assert that iron_print_ast (the printer that powers
 * iron_format_source / `iron fmt` / `textDocument/formatting`) round-trips
 * every v3 construct byte-identically. The fixed-point property is:
 *
 *   iron_print_ast(parse(iron_print_ast(parse(src)))) ==
 *   iron_print_ast(parse(src))                          (byte-for-byte)
 *
 * This is a strictly stronger invariant than "the printed output parses
 * cleanly": it means the printer has reached a stable equivalence class
 * for every v3 fixture. If a v3 AST shape is printable but not
 * re-parseable into the same shape, this test fails.
 *
 * Plus: a generic v2 zero-regression sweep that diffs every non-v3 fixture
 * in tests/integration/ against the Wave 0 baseline snapshot in
 * tests/lsp/fixtures/printer_v2_baseline/. The bit-explicit
 * `is_receiver_form` filter in printer.c is the structural cause; this
 * test is the empirical witness (Pitfall 3 invariant per 09-RESEARCH §3).
 *
 * In Task 1 every assertion is TEST_IGNORE_MESSAGE'd. Tasks 2 and 3
 * progressively flip them to real assertions.
 */

#include "unity.h"

#include "diagnostics/diagnostics.h"
#include "fmt/format.h"
#include "fmt/options.h"
#include "util/arena.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── helpers ──────────────────────────────────────────────────────────── */

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = rd;
    return buf;
}

/* Run iron_format_source once on `src` (length src_len) and return a
 * malloc-owned NUL-terminated copy of the formatted bytes (or NULL on
 * refusal). Caller frees. `len_out` receives the formatted byte length. */
static char *run_format(const char *path, const char *src, size_t src_len,
                         size_t *len_out) {
    (void)src_len;   /* iron_format_source consumes src as a NUL-terminated
                      * string; src_len is kept in the signature so future
                      * callers reading from a buffer with a known length
                      * have a place to assert/bound (Phase 9 ergonomic). */
    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();
    IronFmtOptions opts = iron_fmt_options_default();
    IronFmtResult r = iron_format_source(src, path, &opts, &arena, &diags);
    char *copy = NULL;
    if (r.ok) {
        copy = (char *)malloc(r.formatted_len + 1);
        if (copy) {
            memcpy(copy, r.formatted, r.formatted_len);
            copy[r.formatted_len] = '\0';
            if (len_out) *len_out = r.formatted_len;
        }
    } else if (len_out) {
        *len_out = 0;
    }
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return copy;
}

/* fixed_point_holds: format the source twice (chained) and assert byte
 * equality between the two outputs. Returns 1 on equality, 0 on mismatch.
 * The first output is required to be non-NULL (the fixture must parse). */
static int fixed_point_holds(const char *path) {
    size_t src_len = 0;
    char *src = slurp(path, &src_len);
    if (!src) return 0;

    size_t p1_len = 0;
    char *p1 = run_format(path, src, src_len, &p1_len);
    free(src);
    if (!p1) return 0;

    size_t p2_len = 0;
    char *p2 = run_format(path, p1, p1_len, &p2_len);
    if (!p2) { free(p1); return 0; }

    int eq = (p1_len == p2_len) && (memcmp(p1, p2, p1_len) == 0);
    free(p1);
    free(p2);
    return eq;
}

/* contains: substring search for the canary token sequences D-10 demands. */
static int format_contains(const char *path, const char *needle) {
    size_t src_len = 0;
    char *src = slurp(path, &src_len);
    if (!src) return 0;

    size_t out_len = 0;
    char *out = run_format(path, src, src_len, &out_len);
    free(src);
    if (!out) return 0;

    int hit = (strstr(out, needle) != NULL);
    free(out);
    return hit;
}

#define FIXTURE_DIR TESTS_INTEGRATION_DIR
#define BASELINE_DIR PRINTER_V2_BASELINE_DIR

#define FIXTURE_PATH(name) FIXTURE_DIR "/" name

/* ── v3 fixed-point tests (one per v3 fixture in the corpus) ──────────── */

void test_v3_init_anonymous_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 3 implementation pending");
    /* Real assertion (Task 3):
     *   - format(v3_init_anonymous_and_named.iron) contains "init("
     *   - fixed_point_holds(...) returns 1 */
    TEST_ASSERT_TRUE_MESSAGE(
        format_contains(FIXTURE_PATH("v3_init_anonymous_and_named.iron"), "init("),
        "anonymous init(...) shape");
    TEST_ASSERT_TRUE_MESSAGE(
        fixed_point_holds(FIXTURE_PATH("v3_init_anonymous_and_named.iron")),
        "fixed-point on v3_init_anonymous_and_named.iron");
}

void test_v3_init_named_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 3 implementation pending");
    TEST_ASSERT_TRUE_MESSAGE(
        format_contains(FIXTURE_PATH("v3_init_anonymous_and_named.iron"),
                         "init zero("),
        "named init <name>(...) shape");
    TEST_ASSERT_TRUE_MESSAGE(
        fixed_point_holds(FIXTURE_PATH("v3_init_anonymous_and_named.iron")),
        "fixed-point on v3_init_anonymous_and_named.iron");
}

void test_v3_patch_primitive_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 3 implementation pending");
    TEST_ASSERT_TRUE_MESSAGE(
        format_contains(FIXTURE_PATH("v3_patch_primitive.iron"),
                         "patch object Int"),
        "patch object T prefix");
    TEST_ASSERT_TRUE_MESSAGE(
        fixed_point_holds(FIXTURE_PATH("v3_patch_primitive.iron")),
        "fixed-point on v3_patch_primitive.iron");
}

void test_v3_patch_implements_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 3 implementation pending");
    TEST_ASSERT_TRUE_MESSAGE(
        format_contains(FIXTURE_PATH("v3_patch_implements.iron"),
                         "patch object Int"),
        "patch object T with implements");
    TEST_ASSERT_TRUE_MESSAGE(
        fixed_point_holds(FIXTURE_PATH("v3_patch_implements.iron")),
        "fixed-point on v3_patch_implements.iron");
}

void test_v3_methods_in_block_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 3 implementation pending");
    TEST_ASSERT_TRUE_MESSAGE(
        fixed_point_holds(FIXTURE_PATH("v3_methods_in_block.iron")),
        "fixed-point on v3_methods_in_block.iron");
}

void test_v3_pub_field_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 2 implementation pending");
    /* Real assertion (Task 2):
     *   - format(...) contains "pub var " or "pub val "
     *   - fixed_point_holds returns 1 (passes once Task 3 lands too) */
    int has_pub_field =
        format_contains(FIXTURE_PATH("v3_pub_field_synthesis.iron"), "pub val ") ||
        format_contains(FIXTURE_PATH("v3_pub_field_synthesis.iron"), "pub var ");
    TEST_ASSERT_TRUE_MESSAGE(has_pub_field, "pub var/val prefix on field");
}

void test_v3_readonly_transitive_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 2 implementation pending");
    TEST_ASSERT_TRUE_MESSAGE(
        format_contains(FIXTURE_PATH("v3_readonly_transitive.iron"),
                         "readonly func "),
        "readonly func prefix");
}

void test_v3_pure_method_fixed_point(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 2 implementation pending");
    TEST_ASSERT_TRUE_MESSAGE(
        format_contains(FIXTURE_PATH("v3_pure_method.iron"),
                         "pure func "),
        "pure func prefix");
}

/* ── v2 zero-regression sweep ─────────────────────────────────────────── */

/* Walk tests/lsp/fixtures/printer_v2_baseline/<name>.printed and diff each
 * against fresh iron_format_source output of the matching
 * tests/integration/<name>.iron fixture. Zero diffs is the invariant. */
void test_v2_printer_zero_regression(void) {
    TEST_IGNORE_MESSAGE("Phase 9 Plan 02 Task 3 implementation pending");

    DIR *d = opendir(BASELINE_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, BASELINE_DIR);

    int checked = 0;
    int diffs   = 0;
    char first_diff[512] = {0};

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 9) continue;
        if (strcmp(ent->d_name + nlen - 8, ".printed") != 0) continue;

        char baseline_path[4096];
        snprintf(baseline_path, sizeof(baseline_path), "%s/%s",
                 BASELINE_DIR, ent->d_name);

        char fixture_name[512];
        snprintf(fixture_name, sizeof(fixture_name), "%.*s.iron",
                 (int)(nlen - 8), ent->d_name);
        char fixture_path[4096];
        snprintf(fixture_path, sizeof(fixture_path), "%s/%s",
                 FIXTURE_DIR, fixture_name);

        struct stat st;
        if (stat(fixture_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        size_t src_len = 0;
        char *src = slurp(fixture_path, &src_len);
        if (!src) continue;

        size_t fresh_len = 0;
        char *fresh = run_format(fixture_path, src, src_len, &fresh_len);
        free(src);

        size_t base_len = 0;
        char *base = slurp(baseline_path, &base_len);

        /* Refusal-path semantics: a refused fixture writes empty bytes both
         * at baseline time and post-edit. Treat refusal+empty-baseline as
         * equality. fresh==NULL means the post-edit run also refused. */
        if (!fresh && (!base || base_len == 0)) {
            if (base) free(base);
            checked++;
            continue;
        }

        if (!fresh || !base) {
            diffs++;
            if (first_diff[0] == '\0') {
                snprintf(first_diff, sizeof(first_diff),
                         "%s: refusal/baseline mismatch", fixture_name);
            }
            free(fresh);
            free(base);
            continue;
        }

        if (fresh_len != base_len ||
            memcmp(fresh, base, fresh_len) != 0) {
            diffs++;
            if (first_diff[0] == '\0') {
                snprintf(first_diff, sizeof(first_diff),
                         "%s: %zu fresh bytes vs %zu baseline bytes",
                         fixture_name, fresh_len, base_len);
            }
        }
        free(fresh);
        free(base);
        checked++;
    }
    closedir(d);

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "v2 printer zero-regression: checked=%d diffs=%d first=%s",
             checked, diffs, first_diff[0] ? first_diff : "(none)");
    TEST_ASSERT_TRUE_MESSAGE(checked > 0, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diffs, msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v3_init_anonymous_fixed_point);
    RUN_TEST(test_v3_init_named_fixed_point);
    RUN_TEST(test_v3_patch_primitive_fixed_point);
    RUN_TEST(test_v3_patch_implements_fixed_point);
    RUN_TEST(test_v3_methods_in_block_fixed_point);
    RUN_TEST(test_v3_pub_field_fixed_point);
    RUN_TEST(test_v3_readonly_transitive_fixed_point);
    RUN_TEST(test_v3_pure_method_fixed_point);
    RUN_TEST(test_v2_printer_zero_regression);
    return UNITY_END();
}
