/*
 * title: Broadcast start gate and generation counter
 * topic: concurrency
 * covers: pthread_cond_broadcast, gate opening, waiters count, predicate loops
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 7, ROUNDS = 4 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t gate_cv, ready_cv;
    int waiting;
    int consumed;
    int generation;
    int stop;
    long value; /* published by main each generation */
} Gate;

typedef struct {
    Gate *g;
    int id;
    long acc[ROUNDS];
    int rounds_seen;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *runner(void *p) {
    Arg *a = p;
    Gate *g = a->g;
    int seen = 0;
    pthread_mutex_lock(&g->mu);
    for (;;) {
        g->waiting++;
        pthread_cond_signal(&g->ready_cv);
        while (g->generation == seen && !g->stop)
            pthread_cond_wait(&g->gate_cv, &g->mu);
        g->waiting--;
        if (g->stop)
            break;
        seen = g->generation;
        long v = g->value;
        a->acc[a->rounds_seen++] = v * (a->id + 1);
        g->consumed++;
        pthread_cond_signal(&g->ready_cv);
    }
    pthread_mutex_unlock(&g->mu);
    return NULL;
}

int main(void) {
    Gate g = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER,
              0, 0, 0, 0, 0};
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].g = &g;
        args[i].id = i;
        args[i].rounds_seen = 0;
        check(pthread_create(&th[i], NULL, runner, &args[i]) == 0, "create");
    }
    for (int r = 0; r < ROUNDS; r++) {
        pthread_mutex_lock(&g.mu);
        while (g.waiting < T)
            pthread_cond_wait(&g.ready_cv, &g.mu);
        g.value = (r + 1) * 10;
        g.generation++;
        pthread_cond_broadcast(&g.gate_cv);
        /* wait until everyone has consumed this generation */
        while (g.consumed < T)
            pthread_cond_wait(&g.ready_cv, &g.mu);
        g.consumed = 0;
        pthread_mutex_unlock(&g.mu);
    }
    pthread_mutex_lock(&g.mu);
    while (g.waiting < T)
        pthread_cond_wait(&g.ready_cv, &g.mu);
    g.stop = 1;
    pthread_cond_broadcast(&g.gate_cv);
    pthread_mutex_unlock(&g.mu);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    for (int r = 0; r < ROUNDS; r++) {
        long sum = 0;
        for (int i = 0; i < T; i++) {
            check(args[i].rounds_seen == ROUNDS, "each thread saw each round once");
            sum += args[i].acc[r];
        }
        long expect = (long)(r + 1) * 10 * (T * (T + 1) / 2);
        check(sum == expect, "round sum");
        printf("round %d: weighted sum=%ld\n", r, sum);
    }
    printf("generations=%d\n", g.generation);
    return 0;
}
