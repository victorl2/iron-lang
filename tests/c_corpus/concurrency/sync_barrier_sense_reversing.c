/*
 * title: Sense-reversing spin barrier with lockstep stencil
 * topic: concurrency
 * covers: sense-reversing barrier, C11 atomics, acquire/release, lockstep simulation checked against sequential run
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 5, ROUNDS = 60 };

typedef struct {
    atomic_int remaining;
    atomic_int sense;
    int parties;
} Barrier;

static Barrier bar = {N, 0, N};

static void barrier_wait(Barrier *b, int *local_sense) {
    *local_sense = !*local_sense;
    if (atomic_fetch_sub_explicit(&b->remaining, 1, memory_order_acq_rel) == 1) {
        atomic_store_explicit(&b->remaining, b->parties, memory_order_relaxed);
        atomic_store_explicit(&b->sense, *local_sense, memory_order_release);
    } else {
        while (atomic_load_explicit(&b->sense, memory_order_acquire) != *local_sense)
            sched_yield();
    }
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned cells[N];
static atomic_int arrivals[ROUNDS];
static atomic_int early_pass;

static unsigned step(unsigned l, unsigned c, unsigned r, int round) {
    return (l * 3u + c * 5u + r * 7u + (unsigned)round) % 10007u;
}

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    int sense = 0;
    for (int r = 0; r < ROUNDS; r++) {
        unsigned l = cells[(id + N - 1) % N], c = cells[id], rr = cells[(id + 1) % N];
        unsigned nv = step(l, c, rr, r);
        atomic_fetch_add(&arrivals[r], 1);
        barrier_wait(&bar, &sense); /* everyone has read the old generation */
        cells[id] = nv;
        barrier_wait(&bar, &sense); /* everyone has published the new generation */
        if (atomic_load(&arrivals[r]) != N)
            atomic_fetch_add(&early_pass, 1);
    }
    return NULL;
}

int main(void) {
    unsigned ref[N], tmp[N];
    for (int i = 0; i < N; i++) {
        cells[i] = ref[i] = (unsigned)(i * 37 + 11);
    }
    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < N; i++)
            tmp[i] = step(ref[(i + N - 1) % N], ref[i], ref[(i + 1) % N], r);
        for (int i = 0; i < N; i++)
            ref[i] = tmp[i];
    }

    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    printf("parties=%d rounds=%d barrier episodes=%d\n", N, ROUNDS, 2 * ROUNDS);
    int same = 1;
    for (int i = 0; i < N; i++) {
        printf("cell %d = %u (sequential %u)\n", i, cells[i], ref[i]);
        if (cells[i] != ref[i])
            same = 0;
    }
    printf("early passes %d, matches sequential: %s\n", atomic_load(&early_pass), same ? "yes" : "no");
    check(same, "lockstep result differs from sequential");
    check(atomic_load(&early_pass) == 0, "a thread passed the barrier early");
    check(atomic_load(&bar.remaining) == N, "barrier count reset");
    return 0;
}
