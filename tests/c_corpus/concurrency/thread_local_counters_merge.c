/*
 * title: _Thread_local counters merged at join
 * topic: concurrency
 * covers: _Thread_local, per-thread counters, merge at join, no shared writes
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 5, BUCKETS = 8, ITEMS = 3000 };

static _Thread_local unsigned long tl_hist[BUCKETS];
static _Thread_local unsigned long tl_items;
static _Thread_local unsigned tl_rng;
static unsigned long global_snapshot_main; /* set from main only */

typedef struct {
    int id;
    unsigned long hist[BUCKETS];
    unsigned long items;
    unsigned long tl_seen_at_start;
} Result;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned tl_next(void) {
    tl_rng = tl_rng * 1664525u + 1013904223u;
    return tl_rng >> 8;
}

static void record(unsigned v) {
    tl_hist[v % BUCKETS]++;
    tl_items++;
}

static void *worker(void *p) {
    Result *r = p;
    r->tl_seen_at_start = tl_items; /* each thread starts from a zeroed copy */
    tl_rng = 0x1234u + (unsigned)r->id * 7919u;
    for (int i = 0; i < ITEMS; i++)
        record(tl_next());
    memcpy(r->hist, tl_hist, sizeof tl_hist);
    r->items = tl_items;
    return NULL;
}

int main(void) {
    /* main's own copy is independent of the workers' */
    tl_rng = 0x1234u;
    for (int i = 0; i < 10; i++)
        record(tl_next());
    global_snapshot_main = tl_items;

    Result res[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        memset(&res[i], 0, sizeof res[i]);
        res[i].id = i;
        check(pthread_create(&th[i], NULL, worker, &res[i]) == 0, "create");
    }
    unsigned long merged[BUCKETS] = {0};
    unsigned long items = 0;
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        check(res[i].tl_seen_at_start == 0, "fresh thread-local");
        check(res[i].items == ITEMS, "per-thread items");
        for (int b = 0; b < BUCKETS; b++)
            merged[b] += res[i].hist[b];
        items += res[i].items;
    }
    check(tl_items == global_snapshot_main && tl_items == 10, "main copy unchanged by workers");
    /* recompute each worker's histogram sequentially and compare */
    unsigned long expect[BUCKETS] = {0};
    for (int i = 0; i < T; i++) {
        tl_rng = 0x1234u + (unsigned)i * 7919u;
        for (int k = 0; k < ITEMS; k++) {
            unsigned v = tl_next();
            expect[v % BUCKETS]++;
        }
    }
    unsigned long sum = 0;
    for (int b = 0; b < BUCKETS; b++) {
        check(merged[b] == expect[b], "merged bucket");
        sum += merged[b];
        printf("bucket %d: %lu\n", b, merged[b]);
    }
    check(sum == items && items == (unsigned long)T * ITEMS, "totals");
    printf("items=%lu main-local=%lu\n", items, global_snapshot_main);
    return 0;
}
