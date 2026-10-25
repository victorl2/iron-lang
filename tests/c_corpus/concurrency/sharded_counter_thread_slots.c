/*
 * title: Sharded counter with CAS-claimed thread slots and fold on release
 * topic: concurrency
 * covers: per-thread shard registration, single-writer relaxed increments, fallback shared cell, slot reuse, exact sum
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * A thread claims a shard by CAS(owner, 0 -> myid). The owner is the only writer of that shard,
 * so it increments with a relaxed load+store (no RMW needed). Threads that find every shard
 * taken use a shared fallback cell with fetch_add. On exit a thread folds its shard into `folded`
 * with fetch_add, zeroes the shard, and only then releases ownership, so a later owner starts
 * from 0 and no count is double-counted or lost.
 *
 * Correctness argument: total = folded + fallback + sum(shards) is exact once all threads have
 * joined, because each increment lands in exactly one of those cells and fold moves (not copies)
 * a shard's value before releasing it.
 */
enum { SHARDS = 4, WAVE1 = 7, WAVE2 = 5, INCS = 5000 };

typedef struct {
    alignas(64) atomic_int owner;
    atomic_ulong count;
} Shard;

static Shard shard[SHARDS];
static atomic_ulong folded, fallback;
static atomic_int used_shard, used_fallback;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned amount(int id, int i) {
    return 1u + (unsigned)((id * 31 + i * 17) % 5);
}

static void *worker(void *p) {
    int id = (int)(size_t)p + 1;
    int mine = -1;
    for (int k = 0; k < SHARDS && mine < 0; k++) {
        int s = (id + k) % SHARDS;
        int expect = 0;
        if (atomic_compare_exchange_strong(&shard[s].owner, &expect, id))
            mine = s;
    }
    if (mine >= 0)
        atomic_fetch_add(&used_shard, 1);
    else
        atomic_fetch_add(&used_fallback, 1);
    for (int i = 0; i < INCS; i++) {
        unsigned a = amount(id, i);
        if (mine >= 0) {
            unsigned long c = atomic_load_explicit(&shard[mine].count, memory_order_relaxed);
            atomic_store_explicit(&shard[mine].count, c + a, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&fallback, a, memory_order_relaxed);
        }
        if (i % 500 == 0)
            sched_yield();
    }
    if (mine >= 0) {
        unsigned long c = atomic_exchange_explicit(&shard[mine].count, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&folded, c, memory_order_relaxed);
        atomic_store_explicit(&shard[mine].owner, 0, memory_order_release);
    }
    return NULL;
}

static unsigned long read_total(void) {
    unsigned long t = atomic_load(&folded) + atomic_load(&fallback);
    for (int s = 0; s < SHARDS; s++)
        t += atomic_load(&shard[s].count);
    return t;
}

static unsigned long wave(int first, int n) {
    pthread_t th[WAVE1];
    unsigned long want = 0;
    for (int i = 0; i < n; i++) {
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)(first + i)) == 0, "create");
        for (int k = 0; k < INCS; k++)
            want += amount(first + i + 1, k);
    }
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    return want;
}

int main(void) {
    unsigned long want = wave(0, WAVE1);
    check(read_total() == want, "wave 1 total");
    printf("wave 1: %d threads, total=%lu\n", WAVE1, read_total());
    for (int s = 0; s < SHARDS; s++) {
        check(atomic_load(&shard[s].owner) == 0, "shard released");
        check(atomic_load(&shard[s].count) == 0, "shard folded");
    }
    want += wave(WAVE1, WAVE2);
    check(read_total() == want, "wave 2 total");
    check(atomic_load(&used_shard) >= SHARDS, "shards were used");
    check(atomic_load(&used_shard) + atomic_load(&used_fallback) == WAVE1 + WAVE2, "every thread counted");
    printf("wave 2: %d threads reusing released shards, total=%lu\n", WAVE2, read_total());
    printf("threads accounted: %d\n", atomic_load(&used_shard) + atomic_load(&used_fallback));
    return 0;
}
