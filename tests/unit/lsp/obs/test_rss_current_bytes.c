/* Phase 7 Plan 07-02 Task 01 (HARD-15, D-03) -- Unity tests for the RSS
 * measurement abstraction and cap-trip enforcement.
 *
 * Four RUN_TESTs per plan <behavior>:
 *
 *   Test 1 (Linux procfs parse): inject a mock /proc/self/status text
 *          containing "VmRSS:\t 12345 kB\n"; assert
 *          ilsp_rss_current_bytes() returns 12345 * 1024 = 12,641,280.
 *
 *   Test 2 (procfs malformed tolerance): inject a status text with no
 *          VmRSS line; assert ilsp_rss_current_bytes() returns 0 (the
 *          documented "RSS unknown -> cap never trips" fallback).
 *
 *   Test 3 (cap disabled): ilsp_rss_cap_init(0) returns 0, does NOT mark
 *          installed, does NOT spawn the sampler thread.
 *
 *   Test 4 (cap trips on override): set cap = 1 MiB, inject synthetic
 *          RSS = 2 MiB, call ilsp_rss_sample_and_check_for_testing with
 *          a non-NULL out_tripped; assert the hook reports trip without
 *          _exit-ing and assert the marker file was written.
 */

#include "unity.h"
#include "lsp/obs/rss.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static char s_tmp_xdg[4096];

void setUp(void) {
    /* Each test runs in its own $XDG_STATE_HOME tmpdir so the marker
     * file written by the cap-trip test does not pollute other runs. */
    snprintf(s_tmp_xdg, sizeof(s_tmp_xdg),
             "/tmp/iron-lsp-rss-test-%d-%ld",
             (int)getpid(), (long)time(NULL));
    /* Remove any stale leftover, then create fresh. */
    char path[4200];
    snprintf(path, sizeof(path), "%s/iron-lsp", s_tmp_xdg);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char f[5200];
            snprintf(f, sizeof(f), "%s/%s", path, de->d_name);
            unlink(f);
        }
        closedir(d);
        rmdir(path);
    }
    rmdir(s_tmp_xdg);
    mkdir(s_tmp_xdg, 0700);
    setenv("XDG_STATE_HOME", s_tmp_xdg, 1);

    /* Reset module state between tests so install latches do not
     * carry over. */
    ilsp_rss_reset_for_testing();
    ilsp_rss_set_override_for_testing(0);
    ilsp_rss_set_procfs_override_for_testing(NULL);
}

void tearDown(void) {
    ilsp_rss_set_override_for_testing(0);
    ilsp_rss_set_procfs_override_for_testing(NULL);
    /* Best-effort recursive cleanup of the tmpdir. */
    char path[4200];
    snprintf(path, sizeof(path), "%s/iron-lsp", s_tmp_xdg);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char f[5200];
            snprintf(f, sizeof(f), "%s/%s", path, de->d_name);
            unlink(f);
        }
        closedir(d);
        rmdir(path);
    }
    rmdir(s_tmp_xdg);
}

/* ── Test 1: Linux procfs parse ────────────────────────────────────── */

static void test_procfs_parse_extracts_vmrss_kb(void) {
#if defined(__linux__)
    /* Exactly the canonical VmRSS line from proc(5): kilobytes, a
     * single tab separator, one trailing newline. */
    const char *fake_status =
        "Name:\tironls\n"
        "VmPeak:\t  32768 kB\n"
        "VmSize:\t  20480 kB\n"
        "VmRSS:\t  12345 kB\n"
        "VmData:\t   8192 kB\n";
    ilsp_rss_set_procfs_override_for_testing(fake_status);

    uint64_t got = ilsp_rss_current_bytes();
    /* 12345 KiB * 1024 = 12,641,280 bytes. */
    TEST_ASSERT_EQUAL_UINT64(12345ULL * 1024ULL, got);
#else
    /* Non-Linux platforms: the procfs override is a no-op; the real
     * platform path is exercised by the shell invariant. */
    TEST_IGNORE_MESSAGE("procfs parse test is Linux-only (macOS uses task_info)");
#endif
}

/* ── Test 2: procfs malformed fallback ─────────────────────────────── */

static void test_procfs_parse_missing_vmrss_returns_zero(void) {
#if defined(__linux__)
    /* No VmRSS line at all. Parser should fall back to 0. */
    const char *fake_status =
        "Name:\tironls\n"
        "State:\tR (running)\n"
        "Pid:\t1234\n";
    ilsp_rss_set_procfs_override_for_testing(fake_status);

    uint64_t got = ilsp_rss_current_bytes();
    TEST_ASSERT_EQUAL_UINT64(0ULL, got);
#else
    TEST_IGNORE_MESSAGE("procfs parse test is Linux-only");
#endif
}

/* ── Test 3: IRON_LSP_RSS_CAP_BYTES=0 disables the cap ────────────── */

static void test_cap_init_zero_disables(void) {
    int rc = ilsp_rss_cap_init(0);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(ilsp_rss_cap_installed());
    TEST_ASSERT_EQUAL_UINT64(0ULL, ilsp_rss_cap_bytes());
}

/* ── Test 4: cap trips when synthetic RSS exceeds limit ───────────── */

static void test_cap_trips_on_override(void) {
    /* Install a 1 MiB cap, then override the measurement to 2 MiB so
     * the next synchronous check trips. */
    int rc = ilsp_rss_cap_init(1ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(ilsp_rss_cap_installed());
    TEST_ASSERT_EQUAL_UINT64(1ULL * 1024ULL * 1024ULL,
                             ilsp_rss_cap_bytes());

    ilsp_rss_set_override_for_testing(2ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQUAL_UINT64(2ULL * 1024ULL * 1024ULL,
                             ilsp_rss_current_bytes());

    bool tripped = false;
    int action = ilsp_rss_sample_and_check_for_testing(&tripped);
    TEST_ASSERT_EQUAL_INT(1, action);
    TEST_ASSERT_TRUE_MESSAGE(tripped,
        "cap-trip path should fire when current RSS > cap");

    /* Assert the marker file was written under XDG_STATE_HOME. */
    char marker_dir[5200];
    snprintf(marker_dir, sizeof(marker_dir), "%s/iron-lsp", s_tmp_xdg);
    DIR *d = opendir(marker_dir);
    TEST_ASSERT_NOT_NULL_MESSAGE(d,
        "marker directory $XDG_STATE_HOME/iron-lsp should exist");

    bool found_marker = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "rss-restart-", 12) == 0) {
            found_marker = true;
            break;
        }
    }
    closedir(d);
    TEST_ASSERT_TRUE_MESSAGE(found_marker,
        "expected rss-restart-<iso8601>.log marker under $XDG_STATE_HOME/iron-lsp/");
}

/* ── Test 5: sub-cap reading does NOT trip ────────────────────────── */

static void test_cap_does_not_trip_below_threshold(void) {
    int rc = ilsp_rss_cap_init(4ULL * 1024ULL * 1024ULL);
    TEST_ASSERT_EQUAL_INT(0, rc);
    ilsp_rss_set_override_for_testing(1ULL * 1024ULL * 1024ULL);

    bool tripped = true;   /* seed non-false to prove hook writes it */
    int action = ilsp_rss_sample_and_check_for_testing(&tripped);
    TEST_ASSERT_EQUAL_INT(0, action);
    TEST_ASSERT_FALSE(tripped);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_procfs_parse_extracts_vmrss_kb);
    RUN_TEST(test_procfs_parse_missing_vmrss_returns_zero);
    RUN_TEST(test_cap_init_zero_disables);
    RUN_TEST(test_cap_trips_on_override);
    RUN_TEST(test_cap_does_not_trip_below_threshold);
    return UNITY_END();
}
