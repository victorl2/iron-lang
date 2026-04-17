/* test_cancel_latency.c — HARD-05 cancellation latency invariants.
 *
 * Two tests:
 *   1. Pre-signaled cancel_flag causes immediate return from
 *      iron_analyze_buffer (pipeline-entry poll).
 *   2. A concurrent producer thread flips the cancel flag mid-compile on
 *      a large synthetic source; iron_analyze_buffer returns within the
 *      ctest TIMEOUT, proving poll-site coverage is sufficient to observe
 *      cancellation without hanging.
 *
 * Per Plan 03 / RESEARCH.md Pitfall 2, we measure observable-return shape
 * rather than wall-clock latency (ASan/valgrind can slow the compile by 10-
 * 100x; CI runners have variable wall-clock budgets). "Does not hang" is
 * the robust, platform-independent invariant for HARD-05. */

#include "unity.h"
#include "analyzer/analyzer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(1024 * 1024);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* HARD-05: a pre-signaled cancel_flag causes iron_analyze_buffer to return
 * at the pipeline-entry poll, before lex/parse/analyze runs. The caller
 * observes: empty / partial result + exactly one NOTE-level
 * IRON_ERR_CANCELLED diagnostic. */
void test_pre_signaled_cancel_returns_immediately(void) {
    const char *src = "func main() -> Int { return 0 }\n";
    _Atomic bool cancel = true;

    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, &cancel);

    /* Pipeline never ran — global_scope stays NULL. */
    TEST_ASSERT_NULL(r.global_scope);

    /* Exactly one IRON_ERR_CANCELLED NOTE emitted at the entry. */
    int cancel_notes = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_CANCELLED &&
            diags.items[i].level == IRON_DIAG_NOTE) {
            cancel_notes++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, cancel_notes,
        "pipeline-entry poll must emit exactly one IRON_ERR_CANCELLED NOTE");

    /* Error count unchanged — NOTE level does not bump error_count. */
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
}

/* HARD-05: NULL cancel_flag means "never cancel"; the pipeline runs to
 * completion and no IRON_ERR_CANCELLED is emitted. */
void test_null_cancel_flag_never_fires(void) {
    const char *src = "func main() -> Int { return 0 }\n";

    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL);

    TEST_ASSERT_NOT_NULL(r.global_scope);
    for (int i = 0; i < diags.count; i++) {
        TEST_ASSERT_NOT_EQUAL_INT(IRON_ERR_CANCELLED, diags.items[i].code);
    }
}

/* ── Concurrent-cancel test ──────────────────────────────────────────────── */

typedef struct {
    const char         *src;
    size_t              len;
    _Atomic bool       *cancel;
    Iron_AnalyzeResult  result;
} compile_job_t;

static Iron_Arena    job_arena;
static Iron_DiagList job_diags;

static void *compile_thread_fn(void *arg) {
    compile_job_t *job = (compile_job_t *)arg;
    job->result = iron_analyze_buffer(
        job->src, job->len, "big.iron",
        IRON_ANALYSIS_MODE_CLI,
        &job_arena, &job_diags, job->cancel);
    return NULL;
}

/* HARD-05: setting cancel_flag mid-compile on a large source causes
 * iron_analyze_buffer to return. The invariant we assert is "does not hang"
 * — ctest TIMEOUT in CMakeLists.txt is the wall-clock safety net. */
void test_concurrent_cancel_returns(void) {
    /* Build a ~2000-line valid Iron source (enough work that cancellation
     * during parse is observable — the cancel_flag poll at each statement
     * boundary should fire long before the full program is parsed). */
    const size_t cap = 256 * 1024;
    char *src = (char *)malloc(cap);
    TEST_ASSERT_NOT_NULL(src);

    size_t pos = 0;
    int written = snprintf(src + pos, cap - pos,
                            "func main() -> Int {\n");
    TEST_ASSERT_TRUE(written > 0);
    pos += (size_t)written;

    for (int i = 0; i < 2000 && pos + 64 < cap; i++) {
        written = snprintf(src + pos, cap - pos,
                            "  val x%d = %d\n", i, i);
        if (written <= 0) break;
        pos += (size_t)written;
    }
    written = snprintf(src + pos, cap - pos, "  return 0\n}\n");
    TEST_ASSERT_TRUE(written > 0);
    pos += (size_t)written;

    /* Per-test job arena + diags (fixture arena is reused by setUp/tearDown;
     * using a dedicated pair keeps the main fixture clean for assertions). */
    job_arena = iron_arena_create(2 * 1024 * 1024);
    job_diags = iron_diaglist_create();

    _Atomic bool cancel = false;
    compile_job_t job = {
        .src    = src,
        .len    = pos,
        .cancel = &cancel,
    };

    pthread_t tid;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&tid, NULL, compile_thread_fn, &job));

    /* Yield briefly to let the compile thread enter the pipeline, then
     * signal cancel. Use 1ms sleep — fast enough that the lex+parse+analyze
     * of 2000 lines definitely has not finished yet on any reasonable
     * hardware. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    atomic_store_explicit(&cancel, true, memory_order_relaxed);

    /* Join — test PASSES if we don't hang (ctest TIMEOUT guards us). */
    TEST_ASSERT_EQUAL_INT(0, pthread_join(tid, NULL));

    /* Observable: either the compile raced ahead and finished before the
     * cancel was observed (rare but possible on very fast hardware with
     * large arena budget), OR at least one IRON_ERR_CANCELLED NOTE is
     * present. Both outcomes prove HARD-05 — cancellation did not deadlock.
     *
     * WR-04: the liveness invariant ("cancellation does not hang
     * iron_analyze_buffer") is enforced by ctest TIMEOUT on this test
     * binary in CMakeLists.txt — if pthread_join above hangs, the test
     * runner kills the process and marks the test failed. There is no
     * separate Unity assertion here because both racing outcomes (finished
     * first / cancelled first) are legitimate, so the only useful positive
     * assertion IS the "we got here without TIMEOUT" condition, which ctest
     * already owns. Do not reintroduce TEST_ASSERT_TRUE_MESSAGE(1, ...). */

    iron_diaglist_free(&job_diags);
    iron_arena_free(&job_arena);
    free(src);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pre_signaled_cancel_returns_immediately);
    RUN_TEST(test_null_cancel_flag_never_fires);
    RUN_TEST(test_concurrent_cancel_returns);
    return UNITY_END();
}
