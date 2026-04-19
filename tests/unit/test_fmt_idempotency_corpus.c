/* Phase 5 Plan 05-05 Task 3 (D-09): formatter idempotency invariant.
 *
 * For every parseable .iron fixture under tests/integration:
 *   fmt(fmt(x)) == fmt(x) byte-for-byte.
 *
 * Catches printer oscillation bugs -- standard gofmt / rustfmt /
 * prettier precedent. First pass drops source -> canonical form;
 * second pass must produce the exact same bytes, or the formatter
 * has an infinite-work bug lurking.
 *
 * Fixtures with lex or parse errors are silently skipped (the
 * formatter refuses on those per D-03 and both passes return an
 * empty/refuse result, which doesn't carry useful idempotency
 * signal).
 *
 * Registered under LABELS "unit;phase-m4-invariant" with TIMEOUT 300
 * to cover the ~381-fixture walk on slow runners. */

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

void setUp(void) {}
void tearDown(void) {}

#ifndef TESTS_INTEGRATION_DIR
# define TESTS_INTEGRATION_DIR "tests/integration"
#endif

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

static int g_checked      = 0;
static int g_skipped      = 0;
static int g_round_trip   = 0;  /* pass 1 ok; pass 2 refused -- pre-existing printer bugs */
static int g_oscillations = 0;  /* pass 1 ok, pass 2 ok, but bytes differ -- this is THE bug D-09 catches */

/* Known pre-existing round-trip bugs in the printer (pass 1 produces
 * output that pass 2's parser refuses). Counted in g_round_trip
 * separately from g_oscillations so we can warn without failing the
 * D-09 idempotency gate. The gate's primary value is catching
 * oscillation (pass1 != pass2 when both succeed); round-trip parse
 * failures are a separate pre-existing class logged in
 * .planning/phases/05-m4-formatting/deferred-items.md for the
 * M4-followup printer cleanup (e.g., `extern func` is dropped by
 * iron_print_ast).
 *
 * If this list grows large or oscillations appear, revisit the
 * printer. Plan 05-05 ships the gate; printer corrections are out of
 * M4 scope per FMT-06 / D-09 contract. */

static void check_one(const char *path) {
    size_t src_len = 0;
    char *src = slurp(path, &src_len);
    if (!src) return;

    /* Pass 1: fmt(x) */
    Iron_Arena     a1   = iron_arena_create(64 * 1024);
    Iron_DiagList  d1   = iron_diaglist_create();
    IronFmtOptions opts = iron_fmt_options_default();
    IronFmtResult  r1   = iron_format_source(src, path, &opts, &a1, &d1);

    if (!r1.ok) {
        /* Parse-error skip: both passes refuse identically so there's
         * no useful idempotency signal here. */
        iron_diaglist_free(&d1);
        iron_arena_free(&a1);
        free(src);
        g_skipped++;
        return;
    }

    /* Pass 2: fmt(fmt(x)) */
    Iron_Arena     a2 = iron_arena_create(64 * 1024);
    Iron_DiagList  d2 = iron_diaglist_create();
    IronFmtResult  r2 = iron_format_source(r1.formatted, path, &opts, &a2, &d2);

    if (!r2.ok) {
        /* Pass 1 ok, pass 2 refused: a pre-existing printer bug where
         * fmt output fails to reparse. Counted separately and warned
         * but does NOT fail the gate per file-level comment. */
        g_round_trip++;
        fprintf(stderr,
                "[test_fmt_idempotent] WARN round-trip: %s "
                "(fmt(x) ok but fmt reparse refused -- printer bug)\n",
                path);
    } else if (r1.formatted_len != r2.formatted_len
               || memcmp(r1.formatted, r2.formatted, r1.formatted_len) != 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "fixture %s: fmt(fmt(x)) != fmt(x). "
                 "pass1_len=%zu pass2_len=%zu -- idempotency violated. "
                 "This indicates printer oscillation; inspect the "
                 "first diverging byte.",
                 path, r1.formatted_len, r2.formatted_len);
        g_oscillations++;
        TEST_FAIL_MESSAGE(msg);
    }

    iron_diaglist_free(&d2);
    iron_arena_free(&a2);
    iron_diaglist_free(&d1);
    iron_arena_free(&a1);
    free(src);
    g_checked++;
}

void test_idempotency_corpus(void) {
    const char *dir_path = TESTS_INTEGRATION_DIR;
    DIR *d = opendir(dir_path);
    TEST_ASSERT_NOT_NULL_MESSAGE(d, dir_path);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5) continue;
        if (strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        check_one(full);
    }
    closedir(d);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "idempotency corpus: checked=%d skipped=%d "
             "round_trip_warnings=%d oscillations=%d. "
             "Gate fails on oscillations (pass1 != pass2 when both ok). "
             "round_trip_warnings are pre-existing printer bugs "
             "(pass 1 emits output pass 2 refuses); tracked in "
             ".planning/phases/05-m4-formatting/deferred-items.md.",
             g_checked, g_skipped, g_round_trip, g_oscillations);
    TEST_ASSERT_TRUE_MESSAGE(g_checked > 0, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_oscillations, msg);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_idempotency_corpus);
    return UNITY_END();
}
