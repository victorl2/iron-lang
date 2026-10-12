/*
 * title: Filter lock (generalized Peterson) for N threads
 * topic: concurrency
 * covers: filter lock, levels and victims, waiting rooms, starvation freedom, seq_cst atomics, per-level occupancy bound
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 5, PER = 200 };

static atomic_int level[N];  /* level[i] = highest level thread i is trying to enter */
static atomic_int victim[N]; /* victim[L] = last thread to arrive at level L */
static atomic_int occupancy[N];
static atomic_int over_capacity;

static void filter_lock(int me) {
    for (int L = 1; L < N; L++) {
        atomic_store(&level[me], L);
        atomic_store(&victim[L], me);
        for (;;) {
            int conflict = 0;
            for (int k = 0; k < N && !conflict; k++)
                if (k != me && atomic_load(&level[k]) >= L)
                    conflict = 1;
            if (!conflict || atomic_load(&victim[L]) != me)
                break;
            sched_yield();
        }
        /* At most N-L threads may be past level L at once. */
        int occ = atomic_fetch_add(&occupancy[L], 1) + 1;
        if (occ > N - L)
            atomic_fetch_add(&over_capacity, 1);
    }
}

static void filter_unlock(int me) {
    for (int L = 1; L < N; L++)
        atomic_fetch_sub(&occupancy[L], 1);
    atomic_store(&level[me], 0);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long counter;
static atomic_int in_cs, violations;
static int entries[N];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++) {
        filter_lock(id);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (i % 8 == 0)
            sched_yield();
        counter = v + 1;
        entries[id]++;
        atomic_fetch_sub(&in_cs, 1);
        filter_unlock(id);
    }
    return NULL;
}

int main(void) {
    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);
    printf("levels=%d threads=%d\n", N - 1, N);
    for (int i = 0; i < N; i++)
        printf("thread %d entered %d times\n", i, entries[i]);
    printf("counter=%ld violations=%d\n", counter, atomic_load(&violations));
    printf("Peterson-level occupancy above capacity: %d\n", atomic_load(&over_capacity));
    check(counter == N * PER && atomic_load(&violations) == 0, "exclusion");
    check(atomic_load(&over_capacity) == 0, "level capacity");
    for (int i = 0; i < N; i++)
        check(atomic_load(&level[i]) == 0 && atomic_load(&occupancy[i]) == 0, "idle");
    return 0;
}
