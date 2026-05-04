/* test_help_registry.c — Unity tests for the central CLI help registry
 * (Phase 97 HELP-04 / HELP-05 / HELP-06).
 *
 * Locks Plan 97-01 Task 1's contract from the outside:
 *   - The IRON_CLI_FLAGS array has every flag listed in HELP-05.
 *   - Every entry is well-formed (non-NULL subcommand/flag/description).
 *   - --keep-binary's description mentions 'reserved' so RUN-03's status
 *     is visible in `iron run --help` output once Plan 97-02 wires it.
 *   - Both printer functions emit non-empty output containing the
 *     expected substrings.
 *   - Within each subcommand block of iron_help_print_all, flags appear
 *     in ASCII-alphabetical order (CONTEXT.md "Subcommand help format"
 *     locks this).
 */

#include "unity.h"
#include "cli/help_registry.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Capture helpers ──────────────────────────────────────────────────── */

static void capture_print_all(char *buf, size_t buf_size) {
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    iron_help_print_all("iron", f);
    rewind(f);
    size_t n = fread(buf, 1, buf_size - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static void capture_print_sub(const char *sub, char *buf, size_t buf_size) {
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    iron_help_print_subcommand("iron", sub, f);
    rewind(f);
    size_t n = fread(buf, 1, buf_size - 1, f);
    buf[n] = '\0';
    fclose(f);
}

/* ── 1. count threshold ─────────────────────────────────────────────── */

void test_v97_count_threshold(void) {
    /* Conservative floor: every flag named in HELP-05 + global trio +
     * RUN-03 --keep-binary + init --lib + migrate --from/--to is well over
     * 18, but pick 18 so reasonable additions don't trigger churn. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(18, IRON_CLI_FLAGS_COUNT);
}

/* ── 2. every entry well-formed ─────────────────────────────────────── */

void test_v97_every_entry_well_formed(void) {
    for (int i = 0; i < IRON_CLI_FLAGS_COUNT; i++) {
        const IronCliFlag *f = &IRON_CLI_FLAGS[i];
        TEST_ASSERT_NOT_NULL_MESSAGE(f->subcommand,  "entry.subcommand must be non-NULL (use \"\" for global)");
        TEST_ASSERT_NOT_NULL_MESSAGE(f->flag,         "entry.flag must be non-NULL");
        TEST_ASSERT_NOT_NULL_MESSAGE(f->description,  "entry.description must be non-NULL");
    }
}

/* ── 3. required flags present ──────────────────────────────────────── */

static int registry_has_flag(const char *flag) {
    for (int i = 0; i < IRON_CLI_FLAGS_COUNT; i++) {
        if (strcmp(IRON_CLI_FLAGS[i].flag, flag) == 0) return 1;
    }
    return 0;
}

void test_v97_required_flags_present(void) {
    static const char *required[] = {
        "--release", "--debug-build", "--no-optimize", "--target",
        "--strict-v3", "--no-strict-v3", "--force-comptime",
        "--dump-ir-passes", "--report-compression", "--warn-fusion-break",
        "--verbose", "--output", "--from", "--to", "--lib",
        "--keep-binary", "--help", "--version",
    };
    const int required_count = (int)(sizeof(required) / sizeof(required[0]));
    for (int i = 0; i < required_count; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "registry missing required flag: %s", required[i]);
        TEST_ASSERT_TRUE_MESSAGE(registry_has_flag(required[i]), msg);
    }
}

/* ── 4. --keep-binary reserved ──────────────────────────────────────── */

void test_v97_keep_binary_marked_reserved(void) {
    int found = 0;
    for (int i = 0; i < IRON_CLI_FLAGS_COUNT; i++) {
        if (strcmp(IRON_CLI_FLAGS[i].flag, "--keep-binary") == 0) {
            found = 1;
            TEST_ASSERT_NOT_NULL(strstr(IRON_CLI_FLAGS[i].description, "reserved"));
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "--keep-binary entry not found");
}

/* ── 5. print_subcommand("build") contains --release ────────────────── */

void test_v97_print_build_contains_release(void) {
    char buf[8192];
    capture_print_sub("build", buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strlen(buf) > 0, "print_subcommand(\"build\") emitted empty output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "--release"), "build help missing --release");
}

/* ── 6. print_subcommand("init") contains --lib ─────────────────────── */

void test_v97_print_init_contains_lib(void) {
    char buf[8192];
    capture_print_sub("init", buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strlen(buf) > 0, "print_subcommand(\"init\") emitted empty output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "--lib"), "init help missing --lib");
}

/* ── 7. print_subcommand("migrate") contains --from and --to ────────── */

void test_v97_print_migrate_contains_from_and_to(void) {
    char buf[8192];
    capture_print_sub("migrate", buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strlen(buf) > 0, "print_subcommand(\"migrate\") emitted empty output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "--from"), "migrate help missing --from");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "--to"),   "migrate help missing --to");
}

/* ── 8. print_all contains every subcommand name ────────────────────── */

void test_v97_print_all_contains_every_subcommand(void) {
    char buf[8192];
    capture_print_all(buf, sizeof(buf));
    TEST_ASSERT_TRUE_MESSAGE(strlen(buf) > 0, "print_all emitted empty output");

    static const char *subs[] = {
        "build", "run", "check", "fmt", "test", "init", "migrate",
    };
    const int subs_count = (int)(sizeof(subs) / sizeof(subs[0]));
    for (int i = 0; i < subs_count; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "print_all missing subcommand: %s", subs[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, subs[i]), msg);
    }
}

/* ── 9. print_all alphabetic order within iron build: block ─────────── */

/* Find the first space-or-newline-or-tab terminated token starting at `p`,
 * skipping leading whitespace. Writes up to dst_size-1 chars + '\0' into
 * `dst`. Returns 1 if a token was extracted, 0 otherwise. */
static int extract_first_token(const char *p, char *dst, size_t dst_size) {
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && n + 1 < dst_size) {
        dst[n++] = *p++;
    }
    dst[n] = '\0';
    return n > 0;
}

void test_v97_print_all_alphabetic_within_block(void) {
    char buf[8192];
    capture_print_all(buf, sizeof(buf));

    /* Locate "iron build:" header. */
    char *block_start = strstr(buf, "iron build:");
    TEST_ASSERT_NOT_NULL_MESSAGE(block_start, "print_all is missing iron build: section header");
    /* Advance past the header line. */
    char *line = strchr(block_start, '\n');
    TEST_ASSERT_NOT_NULL(line);
    line++;

    char prev[64] = {0};
    char curr[64];
    int  saw_first = 0;

    while (*line) {
        /* End of section: blank line OR start of next "iron " heading. */
        if (line[0] == '\n') break;
        if (line[0] == 'i' && strncmp(line, "iron ", 5) == 0) break;

        if (extract_first_token(line, curr, sizeof(curr))) {
            /* Skip alias rows (start with single dash, e.g. "-o"). The
             * alphabetic-order contract is over canonical --flag rows. */
            if (curr[0] == '-' && curr[1] != '-') {
                /* alias line, skip */
            } else if (curr[0] == '-' && curr[1] == '-') {
                if (saw_first) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "iron build: flags out of order: '%s' before '%s'", prev, curr);
                    TEST_ASSERT_TRUE_MESSAGE(strcmp(prev, curr) <= 0, msg);
                }
                strncpy(prev, curr, sizeof(prev) - 1);
                prev[sizeof(prev) - 1] = '\0';
                saw_first = 1;
            }
        }

        /* Advance to next line. */
        char *next = strchr(line, '\n');
        if (!next) break;
        line = next + 1;
    }

    TEST_ASSERT_TRUE_MESSAGE(saw_first, "iron build: section had no -- flags");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_v97_count_threshold);
    RUN_TEST(test_v97_every_entry_well_formed);
    RUN_TEST(test_v97_required_flags_present);
    RUN_TEST(test_v97_keep_binary_marked_reserved);
    RUN_TEST(test_v97_print_build_contains_release);
    RUN_TEST(test_v97_print_init_contains_lib);
    RUN_TEST(test_v97_print_migrate_contains_from_and_to);
    RUN_TEST(test_v97_print_all_contains_every_subcommand);
    RUN_TEST(test_v97_print_all_alphabetic_within_block);
    return UNITY_END();
}
