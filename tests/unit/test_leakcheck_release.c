/* test_leakcheck_release.c — Phase 31 Plan 31-03 (DBG-07).
 *
 * Exercises the RELEASE-build opt-in leak check (IRON_LEAK_CHECK). This TU is
 * compiled WITHOUT -DIRON_DEBUG_ALLOCATOR (the release path) and lists the
 * runtime TUs iron_leakcheck.c + iron_heap_track.c + iron_string.c +
 * iron_panic.c directly in its source list so the 16B release header + the
 * side-table register/unregister hooks are compiled ON in THIS executable.
 * (Linking only the iron_runtime archive would still give the release path
 * since the archive is built without the debug define — but listing the TUs
 * directly makes the build-mode intent explicit and matches the Phase 29/30
 * recompile-with-define wiring used by the sibling counter tests.)
 *
 * Coverage map:
 *   - DBG-07 env-ON  — child sets IRON_LEAK_CHECK=1, inits, allocs without
 *                      freeing, exits(0) → atexit dump names the alloc-site
 *                      (1 fork test)
 *   - DBG-07 env-OFF — child does NOT set the env, inits, allocs without
 *                      freeing, exits(0) → stderr is silent (1 fork test)
 *   - DBG-07 no-poison — env ON, alloc + write a sentinel + free; in the SAME
 *                      process assert the freed payload was NOT overwritten
 *                      with 0xDD (release must never poison) (1 in-process test)
 *
 * The fork-per-case helper mirrors tests/unit/test_debug_allocator.c: a child
 * process runs the workload then exit()s normally so the atexit leak dump
 * fires; the parent reads piped child stderr and greps for / asserts the
 * absence of the leak line.
 *
 * POSIX-only (uses fork/pipe/waitpid). Win32 path stubs out with a single
 * TEST_IGNORE_MESSAGE (mirror test_debug_allocator.c lines 29-43).
 */

#include "unity.h"

#ifdef _WIN32

void setUp(void)    {}
void tearDown(void) {}

void test_phase31_leakcheck_release_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 31 DBG-07 leak-check tests are POSIX-only "
                        "(fork/pipe/waitpid/setenv); Win32 path skipped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase31_leakcheck_release_posix_only);
    return UNITY_END();
}

#else  /* POSIX */

#include "runtime/iron_runtime.h"
#include "runtime/iron_heap_track.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── fork-per-case leak-dump capture helper ───────────────────────────────── */

/* Runs child_fn(child_arg) in a child process that is expected to EXIT
 * NORMALLY (so atexit(iron_leakcheck_dump) fires when armed). Captures child
 * stderr into stderr_buf (NUL-terminated). The parent asserts the child exited
 * normally (not signalled). */
static void run_exit_case(void (*child_fn)(void *),
                          void *child_arg,
                          char *stderr_buf,
                          size_t stderr_buf_cap) {
    int err_pipe[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(err_pipe));
    pid_t pid = fork();
    TEST_ASSERT_NOT_EQUAL(-1, pid);
    if (pid == 0) {
        /* child */
        close(err_pipe[0]);
        dup2(err_pipe[1], 2);
        close(err_pipe[1]);
        child_fn(child_arg);
        exit(0);  /* normal exit → atexit handlers (leak dump) fire */
    }
    /* parent */
    close(err_pipe[1]);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < stderr_buf_cap &&
           (n = read(err_pipe[0], stderr_buf + total,
                     stderr_buf_cap - 1 - total)) > 0) {
        total += (size_t)n;
    }
    stderr_buf[total] = '\0';
    close(err_pipe[0]);
    int status = 0;
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, &status, 0));
    TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status),
                             "child must exit normally so atexit fires");
}

/* Child: env ON. Set IRON_LEAK_CHECK=1 BEFORE iron_runtime_init (the env is
 * read once at init), allocate without freeing, exit(0) → the atexit
 * side-table dump must name THIS file as the alloc-site. The literal substring
 * "test_leakcheck_release.c" is the alloc_site_file basename the dump prints. */
static void child_env_on_leak(void *unused) {
    (void)unused;
    setenv("IRON_LEAK_CHECK", "1", 1);
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 48);
    (void)fp;  /* intentionally never freed → leak */
}

/* Child: env OFF. Do NOT set IRON_LEAK_CHECK (and clear any inherited value),
 * init, allocate without freeing, exit(0) → the disarmed side-table must
 * produce a clean, silent exit (no leak line). */
static void child_env_off_leak(void *unused) {
    (void)unused;
    unsetenv("IRON_LEAK_CHECK");
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 48);
    (void)fp;  /* leaked, but the check is disarmed → silent */
}

void test_env_on_dumps_alloc_site(void) {
    char buf[8192];
    run_exit_case(child_env_on_leak, NULL, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "test_leakcheck_release.c"),
        "IRON_LEAK_CHECK=1 leak dump must name the alloc-site file (DBG-07)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "leaked"),
        "IRON_LEAK_CHECK=1 leak dump must announce a leak");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "IRON_LEAK_CHECK"),
        "release leak dump header must tag itself [IRON_LEAK_CHECK]");
}

void test_env_off_is_silent(void) {
    char buf[8192];
    run_exit_case(child_env_off_leak, NULL, buf, sizeof(buf));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "leaked"),
        "without IRON_LEAK_CHECK the release leak check must be silent (DBG-07)");
}

/* ── DBG-07 no-poison: release must NEVER overwrite freed memory with 0xDD ─── */

/* In-process controlled read-after-free in a unit context. With the leak check
 * ARMED (env on) we still must NOT poison in release: after free, the freed
 * payload's bytes are whatever the allocator/libc left — the test only asserts
 * the allocator did NOT deliberately stamp 0xDD over them (the debug-only
 * behaviour). We write a non-0xDD sentinel before free and check the allocator
 * did not replace the whole payload with the 0xDD poison pattern. */
void test_release_does_not_poison(void) {
    setenv("IRON_LEAK_CHECK", "1", 1);
    iron_runtime_init(0, NULL);  /* arms the side-table for this process */

    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 32);
    TEST_ASSERT_NOT_NULL(fp.addr);
    void *user = fp.addr;
    memset(user, 0x11, 32);   /* non-0xDD sentinel across the whole payload */
    iron_heap_free(fp);

    /* Controlled read-after-free: count how many bytes are 0xDD. The release
     * allocator must NOT have poisoned, so the payload must NOT be a full run
     * of 0xDD (it would be if the debug poison ran). A robust check: assert
     * the payload is not entirely 0xDD. */
    int dd_count = 0;
    for (int i = 0; i < 32; i++) {
        if (((volatile unsigned char *)user)[i] == 0xDD) dd_count++;
    }
    TEST_ASSERT_TRUE_MESSAGE(dd_count < 32,
        "release build must NOT poison freed memory with 0xDD (DBG-07: no poison)");
}

/* ── Test runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_env_on_dumps_alloc_site);
    RUN_TEST(test_env_off_is_silent);
    RUN_TEST(test_release_does_not_poison);
    return UNITY_END();
}

#endif  /* !_WIN32 */
