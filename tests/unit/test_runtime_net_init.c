/* test_runtime_net_init.c — Unity tests for iron_runtime_init network
 * initialization hooks (Phase 59 P01c).
 *
 * Covers:
 *   1. test_runtime_init_idempotent
 *      Repeated iron_runtime_init/shutdown pairs must not crash. Per-test
 *      harnesses that call init in setUp rely on this.
 *
 *   2. test_sigpipe_ignored (POSIX only)
 *      After iron_runtime_init(), write() to a closed pipe must return -1
 *      with errno == EPIPE and NOT kill the process. This proves
 *      iron_net_install_sigpipe_ignore() actually installed SIG_IGN.
 *
 *   3. test_wsa_startup_windows (Windows only)
 *      After iron_runtime_init(), socket(AF_INET, SOCK_STREAM, 0) must
 *      succeed — which only happens if WSAStartup was called.
 *
 *   4. test_net_init_decls_exported
 *      The three runtime-init hooks (Iron_net_wsa_startup_once,
 *      Iron_net_wsa_cleanup_once, iron_net_install_sigpipe_ignore) must be
 *      callable as standalone symbols from the header.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <unistd.h>
  #include <signal.h>
  #include <sys/types.h>
#endif

/* ── Unity boilerplate ─────────────────────────────────────────────────────
 * Each test calls iron_runtime_init/shutdown in its own body so the
 * idempotency test can nest pairs without interference.
 */
void setUp(void)    { }
void tearDown(void) { }

/* ── Tests ─────────────────────────────────────────────────────────────── */

void test_runtime_init_idempotent(void) {
    /* Two init / shutdown cycles back-to-back. Should not crash and should
     * leave the runtime in a usable state after each pair. */
    iron_runtime_init(0, NULL);
    iron_runtime_init(0, NULL);  /* second call must be a no-op or refcount bump */

    iron_runtime_shutdown();
    iron_runtime_shutdown();     /* matching second shutdown */

    /* Fresh cycle afterwards proves we didn't leave any dangling state. */
    iron_runtime_init(0, NULL);
    iron_runtime_shutdown();

    TEST_PASS_MESSAGE("iron_runtime_init/shutdown idempotent across repeated calls");
}

#ifndef _WIN32
void test_sigpipe_ignored(void) {
    iron_runtime_init(0, NULL);

    /* Create a pipe and close the read end. Writing to the write end must
     * now return -1 with errno == EPIPE rather than raising SIGPIPE and
     * killing the test process. */
    int fds[2];
    int rc = pipe(fds);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "pipe() failed");

    /* Close the read end so the next write triggers EPIPE. */
    close(fds[0]);

    /* Write a small buffer. If SIG_IGN was not installed, the process
     * dies here with signal 13 (SIGPIPE) and the test never reaches the
     * assertion below. */
    const char buf[] = "x";
    errno = 0;
    ssize_t n = write(fds[1], buf, 1);
    int saved_errno = errno;

    close(fds[1]);

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, (int)n,
        "write() to closed pipe should return -1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(EPIPE, saved_errno,
        "errno should be EPIPE after writing to closed pipe");

    iron_runtime_shutdown();
}
#endif

#ifdef _WIN32
void test_wsa_startup_windows(void) {
    iron_runtime_init(0, NULL);

    /* If WSAStartup was not called, socket() returns INVALID_SOCKET with
     * WSANOTINITIALISED. This check proves Iron_net_wsa_startup_once ran. */
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_NOT_EQUAL_MESSAGE((SOCKET)INVALID_SOCKET, s,
        "socket() failed — WSAStartup not called in iron_runtime_init");
    closesocket(s);

    iron_runtime_shutdown();
}
#endif

void test_net_init_decls_exported(void) {
    /* Compile-time check: grab the function pointers through the symbols
     * declared in iron_runtime.h. If any are missing, this TU will fail
     * to link. */
    int  (*p_startup)(void)  = Iron_net_wsa_startup_once;
    void (*p_cleanup)(void)  = Iron_net_wsa_cleanup_once;
    void (*p_sigpipe)(void)  = iron_net_install_sigpipe_ignore;
    TEST_ASSERT_NOT_NULL(p_startup);
    TEST_ASSERT_NOT_NULL(p_cleanup);
    TEST_ASSERT_NOT_NULL(p_sigpipe);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_runtime_init_idempotent);
#ifndef _WIN32
    RUN_TEST(test_sigpipe_ignored);
#endif
#ifdef _WIN32
    RUN_TEST(test_wsa_startup_windows);
#endif
    RUN_TEST(test_net_init_decls_exported);
    return UNITY_END();
}
