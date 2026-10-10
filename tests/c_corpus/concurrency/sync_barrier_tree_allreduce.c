/*
 * title: Static tree barrier with all-reduce rounds
 * topic: concurrency
 * covers: tree barrier, heap-shaped fan-in, episode counters, release broadcast, all-reduce check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 7, ROUNDS = 40 };

/* Thread i waits for its children (2i+1, 2i+2), reports to its parent, then waits for the release. */
static atomic_int arrived[N];
static atomic_int released;

static void spin_until_ge(atomic_int *v, int target) {
    while (atomic_load_explicit(v, memory_order_acquire) < target)
        sched_yield();
}

static void tree_barrier(int id, int episode) {
    int c1 = 2 * id + 1, c2 = 2 * id + 2;
    if (c1 < N)
        spin_until_ge(&arrived[c1], episode);
    if (c2 < N)
        spin_until_ge(&arrived[c2], episode);
    if (id == 0) {
        atomic_store_explicit(&released, episode, memory_order_release);
    } else {
        atomic_store_explicit(&arrived[id], episode, memory_order_release);
        spin_until_ge(&released, episode);
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long slot[N];
static long sums_seen[N][ROUNDS];

static long contribution(int id, int r) {
    return (long)((id + 1) * (r + 3) % 17) - 5;
}

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    int ep = 0;
    for (int r = 0; r < ROUNDS; r++) {
        slot[id] = contribution(id, r);
        tree_barrier(id, ++ep);
        long s = 0;
        for (int k = 0; k < N; k++)
            s += slot[k];
        sums_seen[id][r] = s;
        tree_barrier(id, ++ep); /* keep slots stable until everyone has summed */
    }
    return NULL;
}

int main(void) {
    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    long grand = 0;
    int mismatches = 0;
    for (int r = 0; r < ROUNDS; r++) {
        long expect = 0;
        for (int k = 0; k < N; k++)
            expect += contribution(k, r);
        for (int i = 0; i < N; i++)
            if (sums_seen[i][r] != expect)
                mismatches++;
        grand += expect;
        if (r < 5 || r == ROUNDS - 1)
            printf("round %2d all-reduce sum %ld\n", r, expect);
    }
    printf("tree of %d nodes, depth 3, %d episodes\n", N, 2 * ROUNDS);
    printf("grand total %ld, mismatching views %d\n", grand, mismatches);
    check(mismatches == 0, "all-reduce mismatch");
    for (int i = 1; i < N; i++)
        check(atomic_load(&arrived[i]) == 2 * ROUNDS, "arrival episodes");
    check(atomic_load(&released) == 2 * ROUNDS, "release episodes");
    return 0;
}
