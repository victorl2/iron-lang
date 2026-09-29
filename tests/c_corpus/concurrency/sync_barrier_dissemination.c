/*
 * title: Dissemination barrier on a ring of threads
 * topic: concurrency
 * covers: dissemination barrier, log2 rounds, partner flags, non power-of-two thread count, lockstep hashing
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 6, LOGN = 3, ROUNDS = 50 };

/* flags[k][i]: episode number signalled to thread i in round k. No resets needed. */
static atomic_int flags[LOGN][N];

static void diss_barrier(int id, int episode) {
    for (int k = 0; k < LOGN; k++) {
        int partner = (id + (1 << k)) % N;
        atomic_store_explicit(&flags[k][partner], episode, memory_order_release);
        while (atomic_load_explicit(&flags[k][id], memory_order_acquire) < episode)
            sched_yield();
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned cell[N];

static unsigned mix(unsigned a, unsigned b, int r) {
    unsigned x = a * 2654435761u ^ (b + 0x9e3779b9u) ^ (unsigned)r * 40503u;
    x ^= x >> 15;
    return x * 2246822519u;
}

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    int ep = 0;
    for (int r = 0; r < ROUNDS; r++) {
        unsigned mine = cell[id], nb = cell[(id + 1) % N];
        unsigned nv = mix(mine, nb, r);
        diss_barrier(id, ++ep);
        cell[id] = nv;
        diss_barrier(id, ++ep);
    }
    return NULL;
}

int main(void) {
    unsigned ref[N], tmp[N];
    for (int i = 0; i < N; i++)
        cell[i] = ref[i] = (unsigned)(i + 1) * 101u;
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < N; i++)
            tmp[i] = mix(ref[i], ref[(i + 1) % N], r);
        for (int i = 0; i < N; i++)
            ref[i] = tmp[i];
    }
    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    int ok = 1;
    for (int i = 0; i < N; i++) {
        printf("cell %d = %08x\n", i, cell[i]);
        if (cell[i] != ref[i])
            ok = 0;
    }
    int rounds_needed = 0;
    while ((1 << rounds_needed) < N)
        rounds_needed++;
    printf("threads=%d dissemination rounds per barrier=%d (LOGN=%d)\n", N, rounds_needed, LOGN);
    printf("matches sequential ring: %s\n", ok ? "yes" : "no");
    check(rounds_needed == LOGN, "round count");
    check(ok, "lockstep mismatch");
    for (int k = 0; k < LOGN; k++)
        for (int i = 0; i < N; i++)
            check(atomic_load(&flags[k][i]) == 2 * ROUNDS, "flag episodes");
    return 0;
}
