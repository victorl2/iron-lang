/* test_double_free.c — Phase 31 Plan 31-01 (DBG-04).
 *
 * Wave 0 RED→GREEN test for double-free detection. This TU is compiled WITH
 * -DIRON_DEBUG_ALLOCATOR (target_compile_definitions in
 * tests/unit/CMakeLists.txt) and lists iron_heap_track.c + iron_panic.c +
 * iron_string.c in its source list so the debug-gated double-free path
 * (iron_heap_free_dbg + iron_panic_double_free) is compiled ON in THIS
 * executable.
 *
 * Coverage: DBG-04 — freeing the same allocation twice reports BOTH the
 * first free-site and the second free-site to stderr, then abort()s.
 *
 * Fork-per-case panic-capture mirrors tests/unit/test_runtime_panic_stale.c:
 * the child allocates, frees, frees AGAIN (the second free triggers the
 * gen-mismatch → iron_panic_double_free → abort path); the parent waits for
 * SIGABRT and greps the captured stderr for BOTH distinct free-site markers.
 *
 * POSIX-only (fork/pipe/waitpid/SIGABRT). Win32 path stubs cleanly.
 */

#include "unity.h"

#ifdef _WIN32

void setUp(void)    {}
void tearDown(void) {}

void test_phase31_double_free_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 31 double-free panic-capture is POSIX-only "
                        "(fork/pipe/waitpid/SIGABRT); Win32 path skipped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase31_double_free_posix_only);
    return UNITY_END();
}

#else  /* POSIX */

#include "runtime/iron_runtime.h"
#include "runtime/iron_heap_track.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* Fork-per-case panic-capture: runs child_fn (which must abort), captures
 * child stderr, asserts SIGABRT. Mirrors test_runtime_panic_stale.c. */
static void run_panic_case(void (*child_fn)(void *),
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
        _exit(99);  /* should never reach: child_fn must abort */
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
    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status),
                             "double-free must abort the process");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIGABRT, WTERMSIG(status),
                                  "double-free must abort via SIGABRT");
}

/* Child: alloc, free at one site, free AGAIN at a different site. The second
 * free is the double-free. We route the two frees through iron_heap_free_dbg
 * so the test can pin two DISTINCT site strings (the real codegen emits
 * iron_heap_free_dbg(fp, __FILE__, __LINE__) under -DIRON_DEBUG_ALLOCATOR;
 * see src/lir/emit_c.c). */
static void child_double_free(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 32);
    Iron_FatPtr fp_copy = fp;
    iron_heap_free_dbg(fp, "first_free_site.iron", 1111);
    /* Second free of the same (now stale) allocation → double-free panic. */
    iron_heap_free_dbg(fp_copy, "second_free_site.iron", 2222);
    /* Unreachable. */
}

void test_double_free_reports_both_sites(void) {
    char buf[4096];
    run_panic_case(child_double_free, NULL, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "first_free_site.iron:1111"),
        "double-free report must name the FIRST free-site");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "second_free_site.iron:2222"),
        "double-free report must name the SECOND free-site");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_double_free_reports_both_sites);
    return UNITY_END();
}

#endif  /* !_WIN32 */
