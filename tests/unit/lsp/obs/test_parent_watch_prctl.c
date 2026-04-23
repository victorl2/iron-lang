/* Phase 7 Plan 07-01 Task 02 (HARD-20, D-08) -- Unity test for the
 * Linux parent-death watcher.
 *
 * Only compiled on Linux (see tests/unit/CMakeLists.txt guard).
 *
 * Scenario: fork a helper child that calls ilsp_parent_watch_init() and
 * then sleeps. The parent of the test process exits; the child should
 * receive SIGTERM within ~5s and exit cleanly. We fork a test-parent
 * (A), which forks test-child (B) that installs the watcher and pauses.
 * A exits (orphaning B); B should be SIGTERM'd by the kernel via
 * PR_SET_PDEATHSIG. A final layer (the actual Unity test process)
 * waitpid()s B with a timeout.
 *
 * Unity layout:
 *   - Test 1: prctl install succeeds + marks installed()
 *   - Test 2: end-to-end signal delivery (fork dance)
 */

#include "unity.h"
#include "lsp/obs/parent_watch.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void setUp(void)    {}
void tearDown(void) {}

static void test_install_reports_installed(void) {
    /* In the main test process, installing the watcher should succeed
     * and the introspection bit should flip. (CAVEAT: the real main
     * test-runner's own parent is the user's shell / ctest; if the
     * shell is alive this install is a no-op beyond setting the bit.) */
    int rc = ilsp_parent_watch_init();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc,
                                  "prctl install returns 0");
    TEST_ASSERT_TRUE(ilsp_parent_watch_installed());

    /* Idempotent: second call is a no-op. */
    rc = ilsp_parent_watch_init();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(ilsp_parent_watch_installed());
}

/* A tiny signal flag updated by SIGTERM handler for the e2e dance. */
static volatile sig_atomic_t s_got_sigterm = 0;
static void sigterm_flag(int signo) { (void)signo; s_got_sigterm = 1; }

static void test_e2e_parent_death_delivers_sigterm(void) {
    /* Design: this Unity process forks a "parent" (P1). P1 forks a
     * "child" (C). C installs sigaction(SIGTERM) + parent_watch_init,
     * then pauses up to 5s. P1 then exits. Kernel should deliver SIGTERM
     * to C. C records that and exits 0; if no SIGTERM arrived, C exits 1.
     * Unity process waitpid's P1, P1 waitpid's C, exit code of C is
     * propagated via stderr pipe to Unity. */
    int status_pipe[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(status_pipe));

    pid_t p1 = fork();
    TEST_ASSERT_MESSAGE(p1 >= 0, "fork p1");
    if (p1 == 0) {
        /* P1 */
        close(status_pipe[0]);
        pid_t c = fork();
        if (c == 0) {
            /* C: install watcher + sigterm trap + pause. Reset the
             * install latch because fork() inherited the Unity
             * process's already-installed state (prctl disposition
             * does NOT inherit across fork on Linux, though). */
            ilsp_parent_watch_reset_for_testing();
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = sigterm_flag;
            sigaction(SIGTERM, &sa, NULL);
            ilsp_parent_watch_init();

            /* Poll up to 5s for the flag; sleep(1) in a loop to let the
             * SIGTERM interrupt it. */
            for (int i = 0; i < 5 && !s_got_sigterm; i++) {
                sleep(1);
            }
            unsigned char ok = s_got_sigterm ? 1 : 0;
            ssize_t ignored = write(status_pipe[1], &ok, 1);
            (void)ignored;
            close(status_pipe[1]);
            _exit(s_got_sigterm ? 0 : 1);
        }
        /* P1: exit immediately so C becomes orphan. Do NOT waitpid(c) --
         * our exit will orphan C, and the kernel delivers SIGTERM. Close
         * the write-side in P1 so C is the only writer. */
        close(status_pipe[1]);
        _exit(0);
    }
    /* Unity: wait for P1 to exit. */
    close(status_pipe[1]);
    int s = 0;
    waitpid(p1, &s, 0);

    /* Read the status byte (or EOF) from the pipe within ~8s. Use a
     * simple polling read with a deadline. */
    unsigned char flag = 0xff;
    time_t deadline = time(NULL) + 8;
    while (time(NULL) < deadline) {
        ssize_t r = read(status_pipe[0], &flag, 1);
        if (r == 1) break;
        if (r == 0) break;   /* EOF; C closed or died */
        usleep(100 * 1000);
    }
    close(status_pipe[0]);

    /* Kill any lingering grandchild so we don't leave zombies. */
    /* (We can't easily know C's pid from here; but since C waits at
     *  most 5s and we gave 8s, it should already be gone.) */

    TEST_ASSERT_EQUAL_MESSAGE(1, flag,
        "orphaned child received SIGTERM within 5s of parent death");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_install_reports_installed);
    RUN_TEST(test_e2e_parent_death_delivers_sigterm);
    return UNITY_END();
}
