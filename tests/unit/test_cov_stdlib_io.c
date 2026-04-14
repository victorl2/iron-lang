/*
 * Phase 69 Plan 04 (COV-05): targeted coverage tests for
 *   - src/stdlib/iron_io.h  (baseline  0.00% line — 0 / 9)
 *   - src/stdlib/iron_io.c  (baseline 22.33% line — 48 / 215)
 *
 * Motivating incident: Phase 69 Plan 03 baseline flagged both files below
 * the 50% line-coverage floor mandated by COV-05. iron_io.h has three
 * static inline wrappers (Iron_io_read_file, Iron_io_list_files,
 * Iron_io_read_bytes) that forward to the *_result variants — llvm-cov
 * records them in whichever translation unit actually inlines them, and
 * no existing test exercises any of them. iron_io.c was partially
 * covered by tests/unit/test_stdlib.c (write_file, read_file_result,
 * file_exists, create_dir, delete_file) but the Phase 39 additions
 * (basename/dirname/join_path/extension/is_dir/append_file/read_lines),
 * the list_files directory walker, and the read_bytes wrappers were all
 * completely untested.
 *
 * Scope: NOT 100% coverage — 50% floor per CONTEXT.md "50% floor, not
 * 100% ceiling". Exercising every uncovered public function with at
 * least one happy-path input is sufficient.
 *
 * Strategy: use /tmp as a sandbox. Create a fresh directory, populate
 * it with a few files, then call every iron_io function against it.
 * Tear down at the end of every test. No mocking required; this is an
 * integration test against a real filesystem.
 *
 * Not tested here: Iron_io_read_line (blocks on stdin — requires pty
 * injection). It covers 4 lines and is not on the critical path for the
 * 50% threshold.
 */

#include "stdlib/iron_io.h"
#include "runtime/iron_runtime.h"
#include "unity.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Unity boilerplate + per-test sandbox ────────────────────────────────── */

/* Use a per-pid subdirectory so parallel ctest runs don't collide. Each
 * test creates files under this directory and tearDown rm -rf's the lot. */
static char   s_sandbox[256];
static size_t s_sandbox_len;

static Iron_String make_str(const char *s) {
    return iron_string_from_cstr(s, strlen(s));
}

/* mkpath joins the sandbox with a relative name and returns a new
 * Iron_String. Caller owns any heap allocation inside the result. */
static Iron_String mkpath(const char *rel) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/%s", s_sandbox, rel);
    return iron_string_from_cstr(buf, strlen(buf));
}

/* Best-effort teardown: unlink every file we might have written under
 * the sandbox, then rmdir the sandbox itself. We only place plain files
 * inside s_sandbox (no nested directories), so a single-level sweep is
 * sufficient. Using direct libc (unlink/rmdir) instead of iron_io to
 * avoid recursive coverage-of-coverage-path. */
static void sandbox_nuke(void) {
    char path[512];
    const char *names[] = {
        "a.txt", "b.txt", "c.txt", "hello.txt", "lines.txt", "append.txt",
        "bytes.bin", "written.txt", "nope.txt", NULL
    };
    for (int i = 0; names[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", s_sandbox, names[i]);
        unlink(path);
    }
    rmdir(s_sandbox);
}

void setUp(void) {
    iron_runtime_init(0, NULL);
    snprintf(s_sandbox, sizeof(s_sandbox),
             "/tmp/iron_cov_io_%d", (int)getpid());
    s_sandbox_len = strlen(s_sandbox);
    /* Create via the iron_io API — exercises Iron_io_create_dir on the
     * happy path and the already-exists branch in one shot. */
    rmdir(s_sandbox);  /* belt & braces from a crashed prior run */
    Iron_String dir = iron_string_from_cstr(s_sandbox, s_sandbox_len);
    Iron_Error err = Iron_io_create_dir(dir);
    TEST_ASSERT_EQUAL_INT(0, err.code);
    /* Second create_dir on the same path must hit the "already exists is
     * not an error" branch — this is the EEXIST arm that tests/unit/
     * test_stdlib.c's test_io_create_dir doesn't currently exercise. */
    err = Iron_io_create_dir(dir);
    TEST_ASSERT_EQUAL_INT(0, err.code);
}

void tearDown(void) {
    sandbox_nuke();
    iron_runtime_shutdown();
}

/* ── iron_io.c: write_file + read_file_result + read_file inline ─────────── */

/* This test covers the iron_io.h inline Iron_io_read_file wrapper (which
 * just forwards to the _result variant) AND exercises the non-error arm
 * of Iron_io_read_file_result's fseek/ftell dance. */
void test_io_read_file_inline_wrapper(void) {
    Iron_String path = mkpath("hello.txt");
    Iron_String body = make_str("hello inline wrapper");

    Iron_Error werr = Iron_io_write_file(path, body);
    TEST_ASSERT_EQUAL_INT(0, werr.code);

    /* The static inline wrapper — this is the only call site the header
     * has that will give iron_io.h any line coverage at all. */
    Iron_String content = Iron_io_read_file(path);
    TEST_ASSERT_EQUAL_STRING("hello inline wrapper", iron_string_cstr(&content));
}

/* ── iron_io.c: read_bytes_result + read_bytes inline wrapper ────────────── */

void test_io_read_bytes_inline_wrapper(void) {
    Iron_String path = mkpath("bytes.bin");
    const uint8_t data[] = {0x01, 0x02, 0x03, 0xff, 0xfe};

    /* Iron_io_write_bytes: not previously tested. */
    Iron_Error werr = Iron_io_write_bytes(path, data, sizeof(data));
    TEST_ASSERT_EQUAL_INT(0, werr.code);

    /* Iron_io_read_bytes_result: uncovered at baseline. */
    Iron_Result_String_Error res = Iron_io_read_bytes_result(path);
    TEST_ASSERT_EQUAL_INT(0, res.v1.code);
    TEST_ASSERT_EQUAL_size_t(sizeof(data), iron_string_byte_len(&res.v0));

    /* Iron_io_read_bytes inline wrapper — gives iron_io.h coverage. */
    Iron_String bytes = Iron_io_read_bytes(path);
    TEST_ASSERT_EQUAL_size_t(sizeof(data), iron_string_byte_len(&bytes));

    /* Write-file failure arm: write to an unwriteable path (/proc/self
     * on Linux) — skipped because it's platform-specific. The happy
     * path is enough for the 50% floor. */
}

/* ── iron_io.c: append_file ──────────────────────────────────────────────── */

void test_io_append_file(void) {
    Iron_String path = mkpath("append.txt");

    /* Seed with an initial write */
    Iron_Error werr = Iron_io_write_file(path, make_str("one\n"));
    TEST_ASSERT_EQUAL_INT(0, werr.code);

    /* Append twice */
    Iron_Error aerr = Iron_io_append_file(path, make_str("two\n"));
    TEST_ASSERT_EQUAL_INT(0, aerr.code);
    aerr = Iron_io_append_file(path, make_str("three\n"));
    TEST_ASSERT_EQUAL_INT(0, aerr.code);

    /* Read back and verify */
    Iron_Result_String_Error res = Iron_io_read_file_result(path);
    TEST_ASSERT_EQUAL_INT(0, res.v1.code);
    TEST_ASSERT_EQUAL_STRING("one\ntwo\nthree\n", iron_string_cstr(&res.v0));

    /* Append to an unopenable path: exercise the error arm */
    Iron_String bad = make_str("/nonexistent/dir/x.txt");
    Iron_Error bad_err = Iron_io_append_file(bad, make_str("..."));
    TEST_ASSERT_NOT_EQUAL_INT(0, bad_err.code);
}

/* ── iron_io.c: list_files_result (the big one) ──────────────────────────── */

void test_io_list_files_happy_path(void) {
    /* Populate the sandbox with three files */
    Iron_io_write_file(mkpath("a.txt"), make_str("alpha"));
    Iron_io_write_file(mkpath("b.txt"), make_str("bravo"));
    Iron_io_write_file(mkpath("c.txt"), make_str("charlie"));

    Iron_String dir = iron_string_from_cstr(s_sandbox, s_sandbox_len);
    Iron_Result_String_Error res = Iron_io_list_files_result(dir);
    TEST_ASSERT_EQUAL_INT(0, res.v1.code);

    /* The output is a newline-separated list of filenames. We don't
     * care about order (readdir is filesystem-defined) — just that all
     * three appear somewhere in the result. */
    const char *out = iron_string_cstr(&res.v0);
    TEST_ASSERT_NOT_NULL(strstr(out, "a.txt"));
    TEST_ASSERT_NOT_NULL(strstr(out, "b.txt"));
    TEST_ASSERT_NOT_NULL(strstr(out, "c.txt"));

    /* And the Iron_io_list_files inline wrapper for iron_io.h coverage. */
    Iron_String via_inline = Iron_io_list_files(dir);
    TEST_ASSERT_NOT_NULL(strstr(iron_string_cstr(&via_inline), "a.txt"));
}

void test_io_list_files_nonexistent(void) {
    /* Error arm: opendir returns NULL */
    Iron_String bad = make_str("/tmp/iron_cov_io_totally_nonexistent_dir");
    Iron_Result_String_Error res = Iron_io_list_files_result(bad);
    TEST_ASSERT_NOT_EQUAL_INT(0, res.v1.code);
}

void test_io_list_files_buffer_growth(void) {
    /* Seed with enough files that the 1024-byte initial buffer is forced
     * to grow at least once. 64 files with 16-byte names ~= 1024 bytes. */
    char rel[32];
    for (int i = 0; i < 80; i++) {
        snprintf(rel, sizeof(rel), "grow_%03d.txt", i);
        Iron_io_write_file(mkpath(rel), make_str("x"));
    }

    Iron_String dir = iron_string_from_cstr(s_sandbox, s_sandbox_len);
    Iron_Result_String_Error res = Iron_io_list_files_result(dir);
    TEST_ASSERT_EQUAL_INT(0, res.v1.code);
    const char *out = iron_string_cstr(&res.v0);
    TEST_ASSERT_NOT_NULL(strstr(out, "grow_000.txt"));
    TEST_ASSERT_NOT_NULL(strstr(out, "grow_079.txt"));

    /* Tear down the extra files so sandbox_nuke can finish rmdir */
    for (int i = 0; i < 80; i++) {
        snprintf(rel, sizeof(rel), "%s/grow_%03d.txt", s_sandbox, i);
        unlink(rel);
    }
}

/* ── iron_io.c: path helpers (Phase 39 additions) ────────────────────────── */

void test_io_basename(void) {
    Iron_String r1 = Iron_io_basename(make_str("/tmp/foo/bar.iron"));
    TEST_ASSERT_EQUAL_STRING("bar.iron", iron_string_cstr(&r1));

    /* No slash in the path: basename returns the whole thing */
    Iron_String r2 = Iron_io_basename(make_str("just-a-name.txt"));
    TEST_ASSERT_EQUAL_STRING("just-a-name.txt", iron_string_cstr(&r2));

    /* Trailing slash — basename returns empty (the char after `/`) */
    Iron_String r3 = Iron_io_basename(make_str("/tmp/foo/"));
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&r3));
}

void test_io_dirname(void) {
    Iron_String r1 = Iron_io_dirname(make_str("/tmp/foo/bar.iron"));
    TEST_ASSERT_EQUAL_STRING("/tmp/foo", iron_string_cstr(&r1));

    /* No slash — returns "." */
    Iron_String r2 = Iron_io_dirname(make_str("bare"));
    TEST_ASSERT_EQUAL_STRING(".", iron_string_cstr(&r2));

    /* Slash at position 0 — returns "/" (the last == p branch) */
    Iron_String r3 = Iron_io_dirname(make_str("/root"));
    TEST_ASSERT_EQUAL_STRING("/", iron_string_cstr(&r3));
}

void test_io_join_path(void) {
    Iron_String j1 = Iron_io_join_path(make_str("/tmp/foo"),
                                        make_str("bar.iron"));
    TEST_ASSERT_EQUAL_STRING("/tmp/foo/bar.iron", iron_string_cstr(&j1));

    /* Trailing slash stripping */
    Iron_String j2 = Iron_io_join_path(make_str("/tmp/foo///"),
                                        make_str("baz.iron"));
    TEST_ASSERT_EQUAL_STRING("/tmp/foo/baz.iron", iron_string_cstr(&j2));
}

void test_io_extension(void) {
    Iron_String e1 = Iron_io_extension(make_str("hello.iron"));
    TEST_ASSERT_EQUAL_STRING("iron", iron_string_cstr(&e1));

    Iron_String e2 = Iron_io_extension(make_str("README"));
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&e2));

    /* Dotfile in filename — no extension */
    Iron_String e3 = Iron_io_extension(make_str(".gitignore"));
    TEST_ASSERT_EQUAL_size_t(0, iron_string_byte_len(&e3));

    /* Multi-dot — keeps the last segment */
    Iron_String e4 = Iron_io_extension(make_str("/path/to/archive.tar.gz"));
    TEST_ASSERT_EQUAL_STRING("gz", iron_string_cstr(&e4));
}

void test_io_is_dir(void) {
    /* True path: the sandbox itself */
    Iron_String dir = iron_string_from_cstr(s_sandbox, s_sandbox_len);
    TEST_ASSERT_TRUE(Iron_io_is_dir(dir));

    /* False path: a plain file */
    Iron_String path = mkpath("a.txt");
    Iron_io_write_file(path, make_str("not a dir"));
    TEST_ASSERT_FALSE(Iron_io_is_dir(path));

    /* Nonexistent path: stat fails, returns false */
    Iron_String missing = mkpath("nonexistent");
    TEST_ASSERT_FALSE(Iron_io_is_dir(missing));
}

void test_io_read_lines(void) {
    Iron_String path = mkpath("lines.txt");
    Iron_io_write_file(path, make_str("line one\nline two\nline three\n"));

    Iron_List_Iron_String lines = Iron_io_read_lines(path);
    /* Trailing \n should have been stripped by the wrapper */
    TEST_ASSERT_EQUAL_INT64(3, lines.count);
    TEST_ASSERT_EQUAL_STRING("line one",   iron_string_cstr(&lines.items[0]));
    TEST_ASSERT_EQUAL_STRING("line two",   iron_string_cstr(&lines.items[1]));
    TEST_ASSERT_EQUAL_STRING("line three", iron_string_cstr(&lines.items[2]));

    /* Read-lines on a nonexistent file should return an empty list. */
    Iron_String missing = mkpath("nope.txt");
    Iron_List_Iron_String empty = Iron_io_read_lines(missing);
    TEST_ASSERT_EQUAL_INT64(0, empty.count);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_io_read_file_inline_wrapper);
    RUN_TEST(test_io_read_bytes_inline_wrapper);
    RUN_TEST(test_io_append_file);
    RUN_TEST(test_io_list_files_happy_path);
    RUN_TEST(test_io_list_files_nonexistent);
    RUN_TEST(test_io_list_files_buffer_growth);
    RUN_TEST(test_io_basename);
    RUN_TEST(test_io_dirname);
    RUN_TEST(test_io_join_path);
    RUN_TEST(test_io_extension);
    RUN_TEST(test_io_is_dir);
    RUN_TEST(test_io_read_lines);
    return UNITY_END();
}
