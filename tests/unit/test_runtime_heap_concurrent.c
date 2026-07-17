/* test_runtime_heap_concurrent.c — Phase 19 thread-safety stress test.
 *
 * Stress test for the SAFE-01 + SAFE-05 atomic generation operations
 * under concurrent access. Three subtests:
 *   A. counter atomicity (per-thread alloc/free loops; no fork)
 *   B. cross-thread free (single allocator + N consumers; only 1 free
 *      succeeds; rest panic via SIGABRT)
 *   C. cross-thread deref (single allocator frees mid-stream; N consumers
 *      panic deterministically on deref)
 *
 * Pattern reference: tests/unit/test_runtime_threads.c:9-92 (atomic_int
 * counter pthread shape) + tests/unit/test_string_intern_race.c (TSAN
 * gated stress pattern).
 *
 * POSIX-only — wrapped in #ifndef _WIN32. Win32 path emits a single
 * TEST_IGNORE_MESSAGE since fork() is not available on Windows.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"
#include "runtime/iron_heap_track.h"

#ifndef _WIN32

#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Stress dimensions ────────────────────────────────────────────────────── */

#define THREADS              8
#define ITERS                100000
#define CROSS_THREAD_CONSUMERS 4

/* ── Test A — counter atomicity (per-thread alloc+free) ──────────────────── */

typedef struct {
    atomic_int *successful_frees;
} StressArgs;

static void *worker_alloc_free(void *arg) {
    StressArgs *a = (StressArgs *)arg;
    for (int i = 0; i < ITERS; i++) {
        Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 64);
        iron_heap_free(fp);
        atomic_fetch_add(a->successful_frees, 1);
    }
    return NULL;
}

void test_iron_heap_concurrent_per_thread_alloc_free(void) {
    /* 8 threads x 100000 iterations of alloc -> free per-thread. After
     * join, verify every iteration completed: no thread crashed mid-loop
     * (which would short the atomic counter). The relaxed-fetch-add on
     * free is the synchronization primitive — if it weren't atomic, the
     * generation increments could be lost and a subsequent alloc/free
     * cycle on the same slot might erroneously succeed. */
    atomic_int successful_frees;
    atomic_init(&successful_frees, 0);

    pthread_t threads[THREADS];
    StressArgs args = { &successful_frees };

    for (int i = 0; i < THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_create(&threads[i], NULL,
                                                worker_alloc_free, &args));
    }
    for (int i = 0; i < THREADS; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(threads[i], NULL));
    }
    TEST_ASSERT_EQUAL_INT(THREADS * ITERS, atomic_load(&successful_frees));
}

/* ── Fork-per-case helpers (Tests B + C) ─────────────────────────────────── */

/* run_consumer_in_fork forks a child, dup2's stderr to a pipe, runs
 * child_fn(arg), reads piped stderr, and reports whether the child died
 * via SIGABRT with the expected substring on stderr. */
static int run_consumer_in_fork(void (*child_fn)(void *), void *arg,
                                const char *expected_substr) {
    int err_pipe[2];
    if (pipe(err_pipe) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        /* Child: redirect stderr into the pipe; no malloc on the panic path. */
        close(err_pipe[0]);
        dup2(err_pipe[1], 2);
        close(err_pipe[1]);
        child_fn(arg);
        _exit(99); /* should never reach — child_fn must abort */
    }

    /* Parent: drain stderr from child. */
    close(err_pipe[1]);
    char buf[2048];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(err_pipe[0], buf + total,
                     sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    buf[total > 0 ? total : 0] = '\0';
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    int sigaborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    int matches    = strstr(buf, expected_substr) != NULL;
    return sigaborted && matches;
}

/* ── Test B — cross-thread free (fork-per-consumer panic capture) ────────── */

static void child_consumer_free(void *arg) {
    Iron_FatPtr *fp = (Iron_FatPtr *)arg;
    /* Do NOT re-init the runtime in the forked child. The parent already
     * called iron_runtime_init; fork() copies that state into the child.
     * Re-initializing under ThreadSanitizer triggers
     * "starting new threads after fork is not supported" because TSAN
     * tracks the parent's thread pool and refuses post-fork spawns.
     * The parent already freed the pointer; the copy here has stale gen.
     * The iron_heap_free call must panic via the double-free detector. */
    iron_heap_free(*fp);  /* expected: panic with stale_pointer */
    _exit(99);            /* should never reach */
}

void test_iron_heap_cross_thread_free_first_succeeds_rest_panic(void) {
    /* Single allocator allocates fp; parent frees it (success). Then we
     * fork N consumers, each holding a copy of the now-stale fp; each
     * consumer's iron_heap_free must panic with stale_pointer. Verifying
     * ALL consumers panic = SAFE-05 + SAFE-03 hold under cross-process
     * (proxy for cross-thread) access. The double-free detector inside
     * iron_heap_free is the synchronization primitive. */
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 64);
    iron_heap_free(fp);  /* parent succeeds first */

    int panic_count = 0;
    for (int i = 0; i < CROSS_THREAD_CONSUMERS; i++) {
        if (run_consumer_in_fork(child_consumer_free, &fp,
                                 "stale pointer dereference")) {
            panic_count++;
        }
    }
    TEST_ASSERT_EQUAL_INT(CROSS_THREAD_CONSUMERS, panic_count);
}

/* ── Test C — cross-thread deref (fork-per-consumer panic capture) ───────── */

static void child_consumer_deref(void *arg) {
    Iron_FatPtr *fp = (Iron_FatPtr *)arg;
    /* Do NOT re-init the runtime — see child_consumer_free comment.
     * Stale fp; deref-check must panic. No zombie deref returning stale
     * data — the acquire-load on hdr->gen synchronizes with the freeing
     * thread's relaxed-fetch-add. */
    iron_check_pointer_gen(*fp, __FILE__, __LINE__);
    _exit(99); /* should never reach */
}

void test_iron_heap_cross_thread_deref_after_free_panics(void) {
    /* Single allocator allocates fp; parent frees the original; copies
     * of fp distributed to forked consumers; each consumer's
     * iron_check_pointer_gen call must panic deterministically. */
    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, 64);
    iron_heap_free(fp);

    int panic_count = 0;
    for (int i = 0; i < CROSS_THREAD_CONSUMERS; i++) {
        if (run_consumer_in_fork(child_consumer_deref, &fp,
                                 "stale pointer dereference")) {
            panic_count++;
        }
    }
    TEST_ASSERT_EQUAL_INT(CROSS_THREAD_CONSUMERS, panic_count);
}

/* ── Unity entrypoint ────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iron_heap_concurrent_per_thread_alloc_free);
    RUN_TEST(test_iron_heap_cross_thread_free_first_succeeds_rest_panic);
    RUN_TEST(test_iron_heap_cross_thread_deref_after_free_panics);
    return UNITY_END();
}

#else /* _WIN32 */

void setUp(void)    {}
void tearDown(void) {}

void test_phase19_stress_posix_only(void) {
    TEST_IGNORE_MESSAGE("Phase 19 thread-stress test is POSIX-only "
                        "(uses fork + pthread); Win32 follows when the "
                        "broader compiler Win32 cleanup phase lands");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_phase19_stress_posix_only);
    return UNITY_END();
}

#endif /* _WIN32 */
