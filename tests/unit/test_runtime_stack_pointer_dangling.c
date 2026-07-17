/* test_runtime_stack_pointer_dangling.c — Phase 20 Plan 02b PTR-10.
 *
 * Wave 0 RED→GREEN tests for the stack-pointer dangling panic. Each
 * RUN_TEST forks a child process that triggers the panic; the parent waits
 * for SIGABRT and reads piped child stderr to assert exact format
 * substrings. Pattern follows tests/unit/test_runtime_panic_stale.c
 * (Phase 19 Plan 19-02 fork-per-case panic-capture).
 *
 * This test file references symbols defined by Plan 20-02b (NOT Plan 20-02a):
 *   - iron_stack_gen — _Thread_local uint64_t TLS slot
 *   - iron_check_stack_pointer_gen — static-inline deref-check helper
 *   - iron_panic_stale_stack_pointer — panic-format helper for stack-frame
 *
 * Plan 20-02a registers this test under CTest LABEL `phase20-pending-20-02b`
 * so it is EXCLUDED from `ctest -L phase20-invariant -LE phase20-pending-20-02b`
 * runs. Plan 20-02b drops the LABEL after wiring the runtime substrate.
 *
 * POSIX-only (uses fork/pipe/waitpid/SIGABRT). Win32 path stubs out.
 */

#include "unity.h"

#ifdef _WIN32

void setUp(void)    {}
void tearDown(void) {}

void test_phase20_stack_panic_capture_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 20 stack-panic tests are POSIX-only "
                        "(fork/pipe/waitpid); Win32 path skipped");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase20_stack_panic_capture_posix_only);
    return UNITY_END();
}

#else  /* POSIX */

#include "runtime/iron_runtime.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Fork-per-case panic-capture helper (mirrors test_runtime_panic_stale.c) */

static void run_panic_case(void (*child_fn)(void *),
                           void *child_arg,
                           char *stderr_buf,
                           size_t stderr_buf_cap,
                           int *status_out) {
    int err_pipe[2];
    TEST_ASSERT_EQUAL_INT(0, pipe(err_pipe));
    pid_t pid = fork();
    TEST_ASSERT_NOT_EQUAL(-1, pid);
    if (pid == 0) {
        close(err_pipe[0]);
        dup2(err_pipe[1], 2);
        close(err_pipe[1]);
        child_fn(child_arg);
        _exit(99);  /* should never reach */
    }
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
    TEST_ASSERT_TRUE(WIFSIGNALED(status));
    TEST_ASSERT_EQUAL_INT(SIGABRT, WTERMSIG(status));
    if (status_out) *status_out = status;
}

/* ── Child workloads — reference Plan 20-02b runtime symbols ──────────── */

/* Single-thread: capture &local in fp; bump iron_stack_gen twice (entry
 * + exit), then deref must panic with "dangling stack pointer to frame". */
static void child_single_thread_dangling(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    int local = 42;
    Iron_FatPtr fp = (Iron_FatPtr){ &local, iron_stack_gen };
    /* Simulate function return: bump TLS counter twice (entry + exit). */
    iron_stack_gen += 1;
    iron_stack_gen += 1;
    /* Now fp.gen != iron_stack_gen → check must panic. */
    iron_check_stack_pointer_gen(fp, "stack_test.c", 4242);
    /* Unreachable. */
}

/* Recursive bump (OQ-E): outer-frame captures &local, inner-frame bumps
 * iron_stack_gen, deref of fp inside inner-frame panics. */
static void child_recursive_bump_dangling(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    int outer_local = 7;
    Iron_FatPtr fp = (Iron_FatPtr){ &outer_local, iron_stack_gen };
    /* Simulate inner frame entry: TLS counter bump. */
    iron_stack_gen += 1;
    /* Deref from inside inner-frame's view: panic expected. */
    iron_check_stack_pointer_gen(fp, "recurse.c", 9999);
    /* Unreachable. */
}

/* No-bump path: function NOT marked takes_local_addr does NOT bump
 * iron_stack_gen; deref of fp captured in same frame does NOT panic.
 * To force a panic outcome (since Unity tests must abort), we cause an
 * intentional abort after the successful no-bump deref. */
static void child_no_bump_no_panic(void *unused) {
    (void)unused;
    iron_runtime_init(0, NULL);
    int local = 99;
    Iron_FatPtr fp = (Iron_FatPtr){ &local, iron_stack_gen };
    /* No bump; deref must succeed (no-op). */
    iron_check_stack_pointer_gen(fp, "no_bump.c", 1);
    /* Force abort after successful no-bump deref so the parent's
     * SIGABRT-expectation matches. The marker substring lets the parent
     * distinguish "successful no-bump" from "panicked stack-pointer". */
    fprintf(stderr, "iron-test: no_bump succeeded, forcing abort\n");
    abort();
}

/* ── Cases ──────────────────────────────────────────────────────────────── */

void test_stack_pointer_panic_after_frame_exit(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_single_thread_dangling, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "dangling stack pointer to frame"),
                                 "stderr must contain stack-pointer panic header");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "stack_test.c:4242"),
                                 "panic must carry caller's deref site");
}

void test_stack_pointer_panic_inside_recursive_inner_frame(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_recursive_bump_dangling, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "dangling stack pointer to frame"),
                                 "recursive inner-frame deref must panic");
}

void test_stack_pointer_no_panic_when_no_bump(void) {
    char buf[4096];
    int  status = 0;
    run_panic_case(child_no_bump_no_panic, NULL, buf, sizeof(buf), &status);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "no_bump succeeded"),
                                 "no-bump deref must succeed before forced abort");
    /* The "dangling stack pointer" header must NOT appear. */
    TEST_ASSERT_NULL_MESSAGE(strstr(buf, "dangling stack pointer to frame"),
                             "no panic header expected when iron_stack_gen unchanged");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_stack_pointer_panic_after_frame_exit);
    RUN_TEST(test_stack_pointer_panic_inside_recursive_inner_frame);
    RUN_TEST(test_stack_pointer_no_panic_when_no_bump);
    return UNITY_END();
}

#endif  /* POSIX */
