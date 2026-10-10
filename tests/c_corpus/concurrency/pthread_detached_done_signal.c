/*
 * title: Detached threads with completion signalling
 * topic: concurrency
 * covers: detached threads, pthread_detach, mutex+condvar completion count, attr detachstate
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
    long results[8];
} Group;

typedef struct {
    Group *g;
    int idx;
    int n;
} Job;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long collatz_steps(long n) {
    long s = 0;
    while (n != 1) {
        n = (n % 2) ? 3 * n + 1 : n / 2;
        s++;
    }
    return s;
}

static void *job_main(void *p) {
    Job *j = p; /* heap-owned by the thread */
    Group *g = j->g;
    long best = 0;
    for (int k = 1; k <= j->n; k++) {
        long s = collatz_steps(k + j->idx * 100);
        if (s > best)
            best = s;
    }
    int idx = j->idx;
    free(j);
    pthread_mutex_lock(&g->mu);
    g->results[idx] = best;
    g->done++;
    pthread_cond_signal(&g->cv);
    pthread_mutex_unlock(&g->mu);
    return NULL;
}

int main(void) {
    Group g;
    pthread_mutex_init(&g.mu, NULL);
    pthread_cond_init(&g.cv, NULL);
    g.done = 0;
    for (int i = 0; i < 8; i++)
        g.results[i] = -1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    check(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0, "setdetach");
    int state = -1;
    check(pthread_attr_getdetachstate(&attr, &state) == 0, "getdetach");
    printf("attr detached: %s\n", state == PTHREAD_CREATE_DETACHED ? "yes" : "no");

    /* first half via attr, second half via pthread_detach */
    for (int i = 0; i < 8; i++) {
        Job *j = malloc(sizeof *j);
        check(j != NULL, "malloc");
        j->g = &g;
        j->idx = i;
        j->n = 100;
        pthread_t t;
        if (i < 4) {
            check(pthread_create(&t, &attr, job_main, j) == 0, "create detached");
        } else {
            check(pthread_create(&t, NULL, job_main, j) == 0, "create");
            check(pthread_detach(t) == 0, "detach");
        }
    }
    pthread_attr_destroy(&attr);

    pthread_mutex_lock(&g.mu);
    while (g.done < 8)
        pthread_cond_wait(&g.cv, &g.mu);
    pthread_mutex_unlock(&g.mu);

    for (int i = 0; i < 8; i++) {
        long expect = 0;
        for (int k = 1; k <= 100; k++) {
            long s = collatz_steps(k + i * 100);
            if (s > expect)
                expect = s;
        }
        check(g.results[i] == expect, "result");
        printf("job %d: longest collatz run %ld\n", i, g.results[i]);
    }
    printf("done=%d\n", g.done);
    pthread_mutex_destroy(&g.mu);
    pthread_cond_destroy(&g.cv);
    return 0;
}
