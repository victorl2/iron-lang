/* test_string_intern_race.c
 *
 * Phase 3 plan-01 (WEB-RUNTIME-01): ThreadSanitizer stress test for
 * iron_string_intern(). Spawns 8 worker threads, each interning a mix of
 * unique-per-worker literals and a single shared hot literal in a tight loop
 * for ~1 second. After all workers finish the main thread asserts the hot
 * literal round-trips correctly.
 *
 * On Linux this binary is built with -fsanitize=thread so any data race
 * detected by tsan causes a non-zero exit (halt_on_error=1 is set via
 * TSAN_OPTIONS env in CI; FAIL_REGULAR_EXPRESSION in CMake catches the
 * "WARNING: ThreadSanitizer" banner even without halt_on_error).
 *
 * On macOS / non-Linux the binary still compiles and runs, providing a plain
 * concurrency smoke test without tsan instrumentation.
 */

#include "unity.h"
#include "runtime/iron_runtime.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/* ── Test parameters ─────────────────────────────────────────────────────── */

#define N_WORKERS           8
#define N_UNIQUE_PER_WORKER 64
#define HOT_LITERAL         "intern_hot_literal_shared_across_workers"

/* Wall-clock budget per worker (nanoseconds → 1 s). Also bounded by a hard
 * iteration cap so slow CI machines don't time-out. */
#define MAX_WALL_NS    1000000000L
#define MAX_ITERATIONS 10000

/* ── Unity boilerplate ───────────────────────────────────────────────────── */

void setUp(void)    { iron_runtime_init(0, NULL); }
void tearDown(void) { iron_runtime_shutdown(); }

/* ── Worker ──────────────────────────────────────────────────────────────── */

static void *worker_fn(void *arg) {
    int worker_id = (int)(intptr_t)arg;

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        /* Check elapsed wall-clock time every 64 iterations to keep the
         * clock_gettime overhead out of the tight inner loop. */
        if ((iter & 63) == 0 && iter > 0) {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            long elapsed_ns = (long)(ts_now.tv_sec  - ts_start.tv_sec)  * 1000000000L
                            + (long)(ts_now.tv_nsec - ts_start.tv_nsec);
            if (elapsed_ns >= MAX_WALL_NS) break;
        }

        /* Intern N_UNIQUE_PER_WORKER distinct literals for this worker. */
        for (int i = 0; i < N_UNIQUE_PER_WORKER; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "uniq_%d_%d", worker_id, i);
            Iron_String s = iron_string_from_cstr(buf, strlen(buf));
            iron_string_intern(s);

            /* Interleave a shared hot literal to maximise contention. */
            Iron_String hot = iron_string_from_cstr(HOT_LITERAL, strlen(HOT_LITERAL));
            iron_string_intern(hot);
        }
    }

    return NULL;
}

/* ── Test function ───────────────────────────────────────────────────────── */

void test_intern_table_race_free(void) {
    pthread_t tids[N_WORKERS];

    /* Spawn workers. */
    for (int i = 0; i < N_WORKERS; i++) {
        int rc = pthread_create(&tids[i], NULL, worker_fn, (void *)(intptr_t)i);
        if (rc != 0) {
            /* pthread_create failure is a hard test error, not a race. */
            TEST_FAIL_MESSAGE("pthread_create failed");
            return;
        }
    }

    /* Wait for all workers to finish. */
    for (int i = 0; i < N_WORKERS; i++) {
        pthread_join(tids[i], NULL);
    }

    /* After all concurrent interning is complete, intern HOT_LITERAL once
     * more on the main thread and assert it round-trips correctly.
     * The real assertion for race-freedom is tsan's own detector — if tsan
     * found a race it will have already printed "WARNING: ThreadSanitizer:
     * data race" (caught by FAIL_REGULAR_EXPRESSION in CMake) or aborted
     * via halt_on_error=1. Reaching here without such output means the
     * single-mutex implementation is race-free. */
    Iron_String hot = iron_string_from_cstr(HOT_LITERAL, strlen(HOT_LITERAL));
    Iron_String interned = iron_string_intern(hot);
    const char *result = iron_string_cstr(&interned);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_EQUAL_STRING(HOT_LITERAL, result);

    /* Explicit pass signal. */
    TEST_ASSERT_TRUE(true);
}

/* ── Unity main ──────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_intern_table_race_free);
    return UNITY_END();
}
