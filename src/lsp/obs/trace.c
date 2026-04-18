/* Phase 2 Plan 06 Task 01 -- Per-request timing histogram.
 *
 * Simple mutex-guarded in-memory aggregator keyed by event name.
 * Designed for shutdown-time summaries: `ilsp_trace_dump()` is called
 * from main.c's teardown path to write the histogram to the structured
 * log (or stderr). The histogram is capped at a small fixed number of
 * distinct names (ILSP_TRACE_MAX_NAMES) -- LSP method names form a
 * bounded set (~20 methods in Phase 2), so a linear scan is faster
 * than a hash map for this use case and keeps the TU dependency-free.
 *
 * Thread-safety: one module-level mutex, initialized via pthread_once
 * under the same pattern as log.c. The global table + mutex is the
 * documented CLAUDE.md exception.
 *
 * Time source: clock_gettime(CLOCK_MONOTONIC) -- not CLOCK_REALTIME --
 * so wall-clock adjustments never produce negative durations. */

#include "lsp/obs/trace.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ILSP_TRACE_MAX_NAMES 32

typedef struct TraceBucket {
    const char *name;       /* caller-owned literal; we store the pointer. */
    uint64_t    count;
    int64_t     sum_ns;
    int64_t     min_ns;
    int64_t     max_ns;
} TraceBucket;

static pthread_once_t  s_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_lock;
static TraceBucket     s_buckets[ILSP_TRACE_MAX_NAMES];
static size_t          s_bucket_count = 0;

static void trace_init_once(void) {
    pthread_mutex_init(&s_lock, NULL);
    memset(s_buckets, 0, sizeof(s_buckets));
    s_bucket_count = 0;
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* Find or allocate a bucket by name. Caller holds the lock. Returns
 * NULL if the table is full (the event is simply not recorded). */
static TraceBucket *find_or_create(const char *name) {
    for (size_t i = 0; i < s_bucket_count; i++) {
        if (s_buckets[i].name == name ||
            (name && s_buckets[i].name && strcmp(s_buckets[i].name, name) == 0)) {
            return &s_buckets[i];
        }
    }
    if (s_bucket_count >= ILSP_TRACE_MAX_NAMES) return NULL;
    TraceBucket *b = &s_buckets[s_bucket_count++];
    b->name    = name;
    b->count   = 0;
    b->sum_ns  = 0;
    b->min_ns  = INT64_MAX;
    b->max_ns  = 0;
    return b;
}

IronLsp_TraceToken ilsp_trace_begin(const char *name) {
    pthread_once(&s_once, trace_init_once);
    IronLsp_TraceToken t;
    t.start_ns = now_ns();
    t.name     = name;
    return t;
}

void ilsp_trace_end(IronLsp_TraceToken t) {
    pthread_once(&s_once, trace_init_once);
    if (!t.name) return;

    int64_t elapsed = now_ns() - t.start_ns;
    if (elapsed < 0) elapsed = 0;

    pthread_mutex_lock(&s_lock);
    TraceBucket *b = find_or_create(t.name);
    if (b) {
        b->count  += 1;
        b->sum_ns += elapsed;
        if (elapsed < b->min_ns) b->min_ns = elapsed;
        if (elapsed > b->max_ns) b->max_ns = elapsed;
    }
    pthread_mutex_unlock(&s_lock);
}

void ilsp_trace_dump(FILE *sink) {
    pthread_once(&s_once, trace_init_once);
    if (!sink) return;

    pthread_mutex_lock(&s_lock);
    for (size_t i = 0; i < s_bucket_count; i++) {
        TraceBucket *b = &s_buckets[i];
        double sum_ms = (double)b->sum_ns / 1.0e6;
        double avg_ms = b->count ? sum_ms / (double)b->count : 0.0;
        double min_ms = (b->min_ns == INT64_MAX)
                      ? 0.0 : (double)b->min_ns / 1.0e6;
        double max_ms = (double)b->max_ns / 1.0e6;
        fprintf(sink,
                "trace %s count=%llu sum_ms=%.3f avg_ms=%.3f min_ms=%.3f max_ms=%.3f\n",
                b->name ? b->name : "(null)",
                (unsigned long long)b->count,
                sum_ms, avg_ms, min_ms, max_ms);
    }
    pthread_mutex_unlock(&s_lock);
}

void ilsp_trace_reset(void) {
    pthread_once(&s_once, trace_init_once);
    pthread_mutex_lock(&s_lock);
    memset(s_buckets, 0, sizeof(s_buckets));
    s_bucket_count = 0;
    pthread_mutex_unlock(&s_lock);
}
