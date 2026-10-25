/*
 * title: Generation-counted condvar barrier with serial thread
 * topic: concurrency
 * covers: reusable barrier, generation counter wraparound, serial-thread election, spurious wakeup safety
 * deps: libc, pthread
 */
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 6, ROUNDS = 200 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int parties, waiting;
    unsigned generation;
} Barrier;

/* Returns 1 for exactly one thread per generation (the last to arrive). */
static int barrier_wait(Barrier *b) {
    pthread_mutex_lock(&b->mu);
    unsigned gen = b->generation;
    if (++b->waiting == b->parties) {
        b->waiting = 0;
        b->generation++; /* wraps around through UINT_MAX on purpose */
        pthread_cond_broadcast(&b->cv);
        pthread_mutex_unlock(&b->mu);
        return 1;
    }
    while (gen == b->generation)
        pthread_cond_wait(&b->cv, &b->mu);
    pthread_mutex_unlock(&b->mu);
    return 0;
}

static Barrier bar = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, N, 0, UINT_MAX - 40u};

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static atomic_int hits[ROUNDS];
static atomic_int serial_per_round[ROUNDS];
static atomic_int early;
static int serial_total[N];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int r = 0; r < ROUNDS; r++) {
        for (int k = 0; k < (id * 7 + r) % 4; k++)
            sched_yield(); /* uneven arrival times */
        atomic_fetch_add(&hits[r], 1);
        int serial = barrier_wait(&bar);
        if (atomic_load(&hits[r]) != N)
            atomic_fetch_add(&early, 1);
        if (serial) {
            atomic_fetch_add(&serial_per_round[r], 1);
            serial_total[id]++;
        }
    }
    return NULL;
}

int main(void) {
    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    int bad_rounds = 0, total = 0;
    for (int r = 0; r < ROUNDS; r++)
        if (atomic_load(&serial_per_round[r]) != 1)
            bad_rounds++;
    for (int i = 0; i < N; i++)
        total += serial_total[i];
    unsigned expected_gen = (UINT_MAX - 40u) + (unsigned)ROUNDS;
    printf("parties=%d rounds=%d\n", N, ROUNDS);
    printf("rounds without exactly one serial thread: %d\n", bad_rounds);
    printf("serial elections total: %d\n", total);
    printf("early passes: %d\n", atomic_load(&early));
    printf("generation wrapped: %s\n", bar.generation < 1000u ? "yes" : "no");
    printf("generation advanced by rounds: %s\n", bar.generation == expected_gen ? "yes" : "no");
    check(bad_rounds == 0, "serial election");
    check(total == ROUNDS, "serial total");
    check(atomic_load(&early) == 0, "early pass");
    check(bar.generation == expected_gen, "generation");
    check(bar.waiting == 0, "waiting reset");
    return 0;
}
