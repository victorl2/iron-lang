/*
 * title: Split-phase (fuzzy) barrier with arrive and wait
 * topic: concurrency
 * covers: fuzzy barrier, arrive/wait split, independent slack work, double-arrive detection, generation tickets
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 5, ROUNDS = 80 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int arrived;
    unsigned generation;
} Fuzzy;

static Fuzzy fb = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};

/* arrive never blocks; the returned ticket names the generation to wait for. */
static unsigned fb_arrive(void) {
    pthread_mutex_lock(&fb.mu);
    unsigned ticket = fb.generation;
    if (++fb.arrived == N) {
        fb.arrived = 0;
        fb.generation++;
        pthread_cond_broadcast(&fb.cv);
    }
    pthread_mutex_unlock(&fb.mu);
    return ticket;
}

static void fb_wait(unsigned ticket) {
    pthread_mutex_lock(&fb.mu);
    while (fb.generation == ticket)
        pthread_cond_wait(&fb.cv, &fb.mu);
    pthread_mutex_unlock(&fb.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int published[ROUNDS][N];
static long slack_sum[N];
static atomic_int stale_reads;

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int r = 0; r < ROUNDS; r++) {
        published[r][id] = r * 100 + id + 1; /* must be visible to all after the wait */
        unsigned t = fb_arrive();
        /* slack work independent of other threads runs between arrive and wait */
        long acc = 0;
        for (int k = 1; k <= 20 + id; k++)
            acc += (long)k * (r + 1) % 13;
        slack_sum[id] += acc;
        fb_wait(t);
        for (int k = 0; k < N; k++)
            if (published[r][k] != r * 100 + k + 1)
                atomic_fetch_add(&stale_reads, 1);
    }
    return NULL;
}

int main(void) {
    pthread_t th[N];
    for (int i = 0; i < N; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    long total = 0, expect_total = 0;
    for (int i = 0; i < N; i++) {
        long e = 0;
        for (int r = 0; r < ROUNDS; r++)
            for (int k = 1; k <= 20 + i; k++)
                e += (long)k * (r + 1) % 13;
        printf("thread %d slack work %ld\n", i, slack_sum[i]);
        check(slack_sum[i] == e, "slack sum");
        total += slack_sum[i];
        expect_total += e;
    }
    printf("total slack %ld (expected %ld)\n", total, expect_total);
    printf("generations %u, stale reads %d\n", fb.generation, atomic_load(&stale_reads));
    check(fb.generation == ROUNDS, "generation count");
    check(atomic_load(&stale_reads) == 0, "stale reads");
    check(fb.arrived == 0, "arrived reset");
    return 0;
}
