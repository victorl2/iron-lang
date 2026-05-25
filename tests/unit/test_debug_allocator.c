/* test_debug_allocator.c — Phase 31 Plan 31-01 (DBG-01/02/03).
 *
 * Wave 0 RED→GREEN tests for the debug heap allocator. This TU is compiled
 * WITH -DIRON_DEBUG_ALLOCATOR (set via target_compile_definitions in
 * tests/unit/CMakeLists.txt) and lists iron_heap_track.c + iron_panic.c +
 * iron_string.c directly in its source list so the 64B debug header,
 * poison-on-free, intrusive registry, and atexit leak dump are compiled
 * ON in THIS executable (linking the release iron_runtime archive alone
 * would NOT turn the define on for those TUs).
 *
 * Coverage map:
 *   - DBG-01 — poison-on-free: a freed payload reads back 0xDD (1 test)
 *   - DBG-02 — alloc-site recorded: leak dump names alloc-site file:line
 *              (covered by the registry-dump grep)
 *   - DBG-03 — intrusive registry + atexit leak dump: alloc-no-free names
 *              the site at exit; alloc+free exits clean (2 fork tests)
 *
 * The fork-per-case helper mirrors tests/unit/test_runtime_panic_stale.c:
 * a child process runs the workload then exit()s normally so the atexit
 * leak dump fires; the parent reads piped child stderr and greps for the
 * site substring (or asserts its absence on the clean-exit case).
 *
 * POSIX-only (uses fork/pipe/waitpid). Win32 path stubs out with a single
 * TEST_IGNORE_MESSAGE (mirror test_runtime_panic_stale.c lines 27-41).
 */

#include "unity.h"

#ifdef _WIN32

void setUp(void)    {}
void tearDown(void) {}

void test_phase31_debug_allocator_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 31 debug-allocator fork tests are POSIX-only "
                        "(fork/pipe/waitpid); Win32 path skipped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase31_debug_allocator_posix_only);
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

/* ── DBG-01: poison-on-free (in-process; controlled UAF read) ─────────────── */

/* Deliberate, controlled use-after-free read in a unit context: after free
 * the freed payload must be filled with 0xDD so any production UAF read hits
 * obvious garbage. This is NOT UB-relying user code — the allocation block is
 * not returned to the OS by free() in any way the test observes other than
 * the poison fill, and we read exactly the byte the allocator wrote. */
void test_poison_on_free_writes_0xDD(void) {
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 64);
    TEST_ASSERT_NOT_NULL(fp.addr);
    void *user = fp.addr;
    memset(user, 0x11, 64);   /* known live pattern */
    iron_heap_free(fp);
    /* Controlled UAF read: the allocator must have poisoned the payload. */
    unsigned char first = ((volatile unsigned char *)user)[0];
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xDD, first,
        "freed payload first byte must read back as 0xDD poison");
}

/* ── DBG-03: fork-per-case leak-dump capture helper ───────────────────────── */

/* Runs child_fn(child_arg) in a child process that is expected to EXIT
 * NORMALLY (so atexit(iron_leak_dump) fires). Captures child stderr into
 * stderr_buf (NUL-terminated). The parent asserts the child exited (not
 * signalled). */
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

/* Child: allocate without freeing, then exit(0) → atexit leak dump must name
 * THIS file as the alloc-site. The literal substring "test_debug_allocator.c"
 * is the alloc_site_file basename the dump prints (DBG-02 provenance). */
static void child_alloc_no_free(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 48);
    (void)fp;  /* intentionally never freed → leak */
}

/* Child: allocate THEN free, then exit(0) → leak dump must report nothing. */
static void child_alloc_then_free(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 48);
    iron_heap_free(fp);
}

void test_leak_dump_names_alloc_site(void) {
    char buf[8192];
    run_exit_case(child_alloc_no_free, NULL, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "test_debug_allocator.c"),
        "atexit leak dump must name the alloc-site file (DBG-02/03)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "leaked"),
        "atexit leak dump must announce a leak");
}

void test_alloc_then_free_clean_exit(void) {
    char buf[8192];
    run_exit_case(child_alloc_then_free, NULL, buf, sizeof(buf));
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "leaked"),
        "alloc+free must produce a clean exit (no leak reported)");
}

/* ── Test runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_poison_on_free_writes_0xDD);
    RUN_TEST(test_leak_dump_names_alloc_site);
    RUN_TEST(test_alloc_then_free_clean_exit);
    return UNITY_END();
}

#endif  /* !_WIN32 */
