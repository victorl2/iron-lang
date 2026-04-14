/* test_runtime_thread_pressure.c — HARDEN-01 (Phase 71) non-abort path tests.
 *
 * **Motivating Incident.** Phase 67-06 commit 85e925c routed every runtime-
 * alloc failure in src/runtime/iron_threads.c through iron_oom_abort,
 * including the 4 IRON_THREAD_CREATE failure sites which are NOT OOM but
 * scheduler pressure (pthread_create EAGAIN). The v1.3.0-alpha milestone
 * audit surfaced this as HARDEN-01 and Phase 71 introduced parallel
 * *_or_error variants for every creation function plus a new
 * Iron_pool_submit_or_error, routing scheduler pressure through a typed
 * IRON_ERR_THREAD_LIMIT return instead of aborting the host process.
 *
 * **Test Strategy.** Three test cases, all fork-based where an abort is
 * possible so the child's exit does not kill the test binary:
 *
 *   1. test_mutex_create_or_error_oom_aborts
 *        Call Iron_mutex_create_or_error(NULL, SIZE_MAX). The value-side
 *        malloc(SIZE_MAX) is guaranteed to return NULL on every platform
 *        (the allocator's own overflow check trips — no amount of macOS
 *        virtual-address overcommit can satisfy SIZE_MAX bytes). The
 *        _or_error body still iron_oom_aborts on true malloc-NULL
 *        (OOM stays a hard abort per HARDEN-01 design). The child
 *        subprocess must exit via SIGABRT with the expected "iron: out
 *        of memory at" stderr prefix and the "Iron_mutex_create_or_error"
 *        location literal. Proves the OOM path is unchanged across the
 *        Plan 02/03/04 migration. We pick Iron_mutex_create_or_error
 *        rather than Iron_elastic_pool_create_or_error because its
 *        malloc takes a size_t parameter directly, so we can pass
 *        SIZE_MAX without any multiplication-rounding or int32_t
 *        clamping — deterministic NULL on glibc, musl, and Darwin libc.
 *
 *   2. test_pool_create_or_error_thread_limit (Linux-only)
 *        Fork a subprocess, call setrlimit(RLIMIT_NPROC, {N,N}) where N
 *        is just above the current thread count, then call
 *        Iron_pool_create_or_error("pressure", 16). The subsequent
 *        pthread_create calls return EAGAIN. Assert r.pool == NULL,
 *        r.err.code == IRON_ERR_THREAD_LIMIT, and the subprocess reaches
 *        _exit(0) WITHOUT aborting. Proves the load-bearing HARDEN-01
 *        fix works: scheduler pressure routes through a typed error
 *        instead of terminating the host.
 *        Skipped on non-Linux via Unity's TEST_IGNORE_MESSAGE.
 *
 *   3. test_pool_submit_or_error_happy_path
 *        Sanity check. Create a 2-thread pool via legacy Iron_pool_create,
 *        call Iron_pool_submit_or_error(pool, noop_fn, NULL) five times,
 *        assert all 5 returns are iron_error_is_ok(), drain via
 *        Iron_pool_barrier, destroy via Iron_pool_destroy. Proves the new
 *        public submit entry point shares state correctly with the legacy
 *        creation/barrier/destroy path.
 *
 * **Severity.** M-H — without this test, the HARDEN-01 regression guarantee
 * relies entirely on code review of Plans 02-04. With this test, the
 * scheduler-pressure fix has a deterministic CI canary on Linux.
 */

#define _GNU_SOURCE
#include "runtime/iron_runtime.h"
#include "runtime/iron_errors.h"
#include "unity.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#  include <sys/resource.h>
#  include <sys/time.h>
#endif

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* No-op worker for the happy-path submit test. */
static void noop_fn(void *arg) {
    (void)arg;
}

/* ── Test 1: OOM path still aborts ──────────────────────────────────────── */

void test_mutex_create_or_error_oom_aborts(void) {
    int stderr_pipe[2];
    TEST_ASSERT_EQUAL(0, pipe(stderr_pipe));

    pid_t child = fork();
    TEST_ASSERT_TRUE(child >= 0);

    if (child == 0) {
        /* Child: request a mutex with value_size = SIZE_MAX. The value-side
         * malloc(SIZE_MAX) inside Iron_mutex_create_or_error is guaranteed
         * to return NULL on every libc — glibc, musl, and Darwin libc all
         * reject allocations >= SIZE_MAX. No amount of macOS lazy-commit
         * virtual-memory magic can satisfy the request, because the libc
         * allocator's own arithmetic overflow / ceiling check trips before
         * the kernel is ever asked. iron_oom_abort fires from the NULL
         * check and prints "iron: out of memory at iron_threads.c:
         * Iron_mutex_create_or_error value" to stderr before abort(3). */
        close(stderr_pipe[0]);
        dup2(stderr_pipe[1], 2);

        Iron_Mutex_OrError r =
            Iron_mutex_create_or_error(NULL, (size_t)-1 /* SIZE_MAX */);
        (void)r;
        _exit(99);  /* unreached */
    }

    close(stderr_pipe[1]);

    char buf[512] = {0};
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(stderr_pipe[0], buf + total,
                          sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(child, &status, 0);

    TEST_ASSERT_TRUE_MESSAGE(WIFSIGNALED(status),
        "child must exit via signal (SIGABRT) on OOM");
    TEST_ASSERT_EQUAL_MESSAGE(SIGABRT, WTERMSIG(status),
        "child must receive SIGABRT from iron_oom_abort on OOM");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "iron: out of memory at"),
        "stderr must contain iron_oom_abort prefix on OOM");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(buf, "Iron_mutex_create_or_error"),
        "stderr must identify the _or_error variant's location literal");
}

/* ── Test 2: scheduler pressure returns IRON_ERR_THREAD_LIMIT (Linux) ──── */

void test_pool_create_or_error_thread_limit(void) {
#ifndef __linux__
    TEST_IGNORE_MESSAGE("RLIMIT_NPROC scheduler-pressure test is Linux-only — "
                        "macOS setrlimit behavior is not deterministic enough");
#else
    int report_pipe[2];
    TEST_ASSERT_EQUAL(0, pipe(report_pipe));

    pid_t child = fork();
    TEST_ASSERT_TRUE(child >= 0);

    if (child == 0) {
        /* Child: clamp RLIMIT_NPROC tightly. We ask for 16 worker threads
         * which far exceeds the clamp, guaranteeing pthread_create returns
         * EAGAIN on the first or second iteration of the IRON_THREAD_CREATE
         * loop inside Iron_pool_create_or_error. Some glibc versions
         * require rlim >= 2 to account for the ld.so helper thread; we use
         * 2 which is as tight as portably possible. */
        close(report_pipe[0]);

        struct rlimit rl = { .rlim_cur = 2, .rlim_max = 2 };
        if (setrlimit(RLIMIT_NPROC, &rl) != 0) {
            dprintf(report_pipe[1], "SETRLIMIT_FAILED errno=%d", errno);
            close(report_pipe[1]);
            _exit(2);
        }

        Iron_Pool_OrError r = Iron_pool_create_or_error("pressure", 16);

        if (r.pool != NULL) {
            dprintf(report_pipe[1], "UNEXPECTED_POOL_NONNULL");
            close(report_pipe[1]);
            _exit(3);
        }
        if (r.err.code == 0) {
            dprintf(report_pipe[1], "UNEXPECTED_ERR_CODE_ZERO");
            close(report_pipe[1]);
            _exit(4);
        }
        /* Report the error code so the parent can verify it. */
        dprintf(report_pipe[1], "OK code=%d", r.err.code);
        close(report_pipe[1]);
        _exit(0);
    }

    close(report_pipe[1]);

    char buf[128] = {0};
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(report_pipe[0], buf + total,
                          sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    close(report_pipe[0]);

    int status = 0;
    waitpid(child, &status, 0);

    /* The child MUST NOT abort. If it did, HARDEN-01 is regressed. */
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
        TEST_FAIL_MESSAGE(
            "Iron_pool_create_or_error aborted the subprocess under RLIMIT_NPROC "
            "pressure — HARDEN-01 regression: scheduler pressure must NOT route "
            "through iron_oom_abort");
    }

    /* If setrlimit itself failed (some CI runners deny lowering RLIMIT_NPROC
     * below the current ceiling via seccomp / rootless containers), treat
     * this as IGNORE rather than FAIL. The test is only meaningful when the
     * clamp actually applies. */
    if (strncmp(buf, "SETRLIMIT_FAILED", 16) == 0) {
        TEST_IGNORE_MESSAGE(buf);
    }

    TEST_ASSERT_TRUE_MESSAGE(WIFEXITED(status),
        "child must exit cleanly (not via signal) when Iron_pool_create_or_error "
        "returns IRON_ERR_THREAD_LIMIT");

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(status),
        "child must _exit(0) after successfully observing "
        "r.pool==NULL && r.err.code==IRON_ERR_THREAD_LIMIT");

    /* Verify the reported error code matches IRON_ERR_THREAD_LIMIT (7000). */
    char expected[64];
    snprintf(expected, sizeof(expected), "OK code=%d", IRON_ERR_THREAD_LIMIT);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, expected),
        "subprocess must report IRON_ERR_THREAD_LIMIT (7000) as the returned "
        "err.code");
#endif
}

/* ── Test 3: Iron_pool_submit_or_error happy path ──────────────────────── */

void test_pool_submit_or_error_happy_path(void) {
    /* Legacy Iron_pool_create internally delegates to _or_error and aborts
     * on failure; on the happy path it returns a live pool. We then submit
     * via the new _or_error entry point and verify each return is ok. */
    Iron_Pool *pool = Iron_pool_create("happy", 2);
    TEST_ASSERT_NOT_NULL(pool);

    for (int i = 0; i < 5; i++) {
        Iron_Error e = Iron_pool_submit_or_error(pool, noop_fn, NULL);
        TEST_ASSERT_TRUE_MESSAGE(iron_error_is_ok(e),
            "Iron_pool_submit_or_error should succeed on a healthy pool");
    }

    /* Wait for all 5 work items to complete before destroy so the
     * destroy path does not race with the workers. */
    Iron_pool_barrier(pool);
    Iron_pool_destroy(pool);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mutex_create_or_error_oom_aborts);
    RUN_TEST(test_pool_create_or_error_thread_limit);
    RUN_TEST(test_pool_submit_or_error_happy_path);
    return UNITY_END();
}
