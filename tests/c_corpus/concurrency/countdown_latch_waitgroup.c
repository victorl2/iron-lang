/*
 * title: Countdown latch, wait group and chained phase latches
 * topic: concurrency
 * covers: one-shot latch, start gate, wait group with dynamic add, phase latches as barrier chain, visibility of prior phase
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    long count;
} Latch;

static void latch_init(Latch *l, long n) {
    pthread_mutex_init(&l->mu, NULL);
    pthread_cond_init(&l->cv, NULL);
    l->count = n;
}

static void latch_destroy(Latch *l) {
    pthread_mutex_destroy(&l->mu);
    pthread_cond_destroy(&l->cv);
}

static void latch_count_down(Latch *l) {
    pthread_mutex_lock(&l->mu);
    if (l->count > 0)
        l->count--;
    if (l->count == 0)
        pthread_cond_broadcast(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

static void latch_wait(Latch *l) {
    pthread_mutex_lock(&l->mu);
    while (l->count > 0)
        pthread_cond_wait(&l->cv, &l->mu);
    pthread_mutex_unlock(&l->mu);
}

/* A wait group is a latch that can grow while it is open. */
static void wg_add(Latch *l, long n) {
    pthread_mutex_lock(&l->mu);
    l->count += n;
    pthread_mutex_unlock(&l->mu);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Part A: start gate */
enum { NA = 6 };
static Latch ready, gate, finished;
static int gate_was_open[NA];
static int gate_flag;
static pthread_mutex_t flag_mu = PTHREAD_MUTEX_INITIALIZER;

static void *gated(void *arg) {
    int id = (int)(long)arg;
    latch_count_down(&ready);
    latch_wait(&gate);
    pthread_mutex_lock(&flag_mu);
    gate_was_open[id] = gate_flag;
    pthread_mutex_unlock(&flag_mu);
    latch_count_down(&finished);
    return NULL;
}

/* Part B: recursive wait group. Every task adds its children to the group before finishing. */
static Latch wg;
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t tthreads[128];
static int ntthreads;
static long node_sum, node_count;

static void spawn(int depth, long path);

static void *tree_task(void *arg) {
    long *p = arg;
    int depth = (int)(p[0]);
    long path = p[1];
    free(p);
    pthread_mutex_lock(&tmu);
    node_sum += path;
    node_count++;
    pthread_mutex_unlock(&tmu);
    if (depth < 4) {
        wg_add(&wg, 2);
        spawn(depth + 1, path * 2);
        spawn(depth + 1, path * 2 + 1);
    }
    latch_count_down(&wg);
    return NULL;
}

static void spawn(int depth, long path) {
    long *p = malloc(2 * sizeof *p);
    check(p != NULL, "alloc");
    p[0] = depth;
    p[1] = path;
    pthread_mutex_lock(&tmu);
    check(ntthreads < 128, "thread slots");
    int slot = ntthreads++;
    pthread_mutex_unlock(&tmu);
    /* the thread id is written under tmu by the creator; joined only after the wait group opens */
    pthread_t t;
    check(pthread_create(&t, NULL, tree_task, p) == 0, "create");
    pthread_mutex_lock(&tmu);
    tthreads[slot] = t;
    pthread_mutex_unlock(&tmu);
}

/* Part C: three phases separated by latches; a phase may read all results of the previous one. */
enum { NC = 5, PHASES = 3 };
static Latch phase_latch[PHASES];
static long phase_val[PHASES][NC];

static void *phased(void *arg) {
    int id = (int)(long)arg;
    for (int ph = 0; ph < PHASES; ph++) {
        long v = (id + 1) * (ph + 2);
        if (ph > 0) {
            long s = 0;
            for (int k = 0; k < NC; k++)
                s += phase_val[ph - 1][k];
            v += s; /* needs every worker's previous value */
        }
        phase_val[ph][id] = v;
        latch_count_down(&phase_latch[ph]);
        latch_wait(&phase_latch[ph]);
    }
    return NULL;
}

int main(void) {
    latch_init(&ready, NA);
    latch_init(&gate, 1);
    latch_init(&finished, NA);
    pthread_t ta[NA];
    for (long i = 0; i < NA; i++)
        check(pthread_create(&ta[i], NULL, gated, (void *)i) == 0, "create gated");
    latch_wait(&ready);
    pthread_mutex_lock(&flag_mu);
    gate_flag = 1;
    pthread_mutex_unlock(&flag_mu);
    latch_count_down(&gate);
    latch_wait(&finished);
    for (int i = 0; i < NA; i++)
        pthread_join(ta[i], NULL);
    int open_seen = 0;
    for (int i = 0; i < NA; i++)
        open_seen += gate_was_open[i];
    check(open_seen == NA, "no worker passed before the gate opened");
    printf("start gate: %d of %d workers saw the gate open\n", open_seen, NA);
    latch_destroy(&ready);
    latch_destroy(&gate);
    latch_destroy(&finished);

    latch_init(&wg, 1);
    spawn(0, 1);
    latch_wait(&wg);
    for (int i = 0; i < ntthreads; i++)
        pthread_join(tthreads[i], NULL);
    check(node_count == 31, "31 nodes in a depth-4 binary tree");
    long expect = 0;
    for (long d = 0, first = 1; d <= 4; d++, first *= 2)
        for (long k = 0; k < first; k++)
            expect += first + k;
    check(node_sum == expect, "sum of heap-style paths");
    printf("wait group: %ld tasks, path sum %ld, threads joined %d\n", node_count, node_sum, ntthreads);
    latch_destroy(&wg);

    pthread_t tc[NC];
    for (int i = 0; i < PHASES; i++)
        latch_init(&phase_latch[i], NC);
    for (long i = 0; i < NC; i++)
        check(pthread_create(&tc[i], NULL, phased, (void *)i) == 0, "create phased");
    for (int i = 0; i < NC; i++)
        pthread_join(tc[i], NULL);
    long ref[PHASES][NC];
    for (int ph = 0; ph < PHASES; ph++) {
        long s = 0;
        if (ph > 0)
            for (int k = 0; k < NC; k++)
                s += ref[ph - 1][k];
        for (int id = 0; id < NC; id++)
            ref[ph][id] = (id + 1) * (ph + 2) + s;
    }
    for (int ph = 0; ph < PHASES; ph++) {
        long tot = 0;
        for (int id = 0; id < NC; id++) {
            check(phase_val[ph][id] == ref[ph][id], "phase values");
            tot += phase_val[ph][id];
        }
        printf("phase %d total %ld\n", ph, tot);
        latch_destroy(&phase_latch[ph]);
    }
    return 0;
}
