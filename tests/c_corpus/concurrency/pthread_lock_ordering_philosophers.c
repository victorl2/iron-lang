/*
 * title: Dining philosophers with global lock ordering
 * topic: concurrency
 * covers: deadlock avoidance, lock ordering by index, many mutexes, meal accounting
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 5, MEALS = 300 };

static pthread_mutex_t fork_mu[N];
static int fork_owner[N]; /* -1 when free; only touched while holding fork_mu[i] */

typedef struct {
    int id;
    int meals;
    long ate_units;
    int overlap_bad;
} Phil;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *philosopher(void *p) {
    Phil *ph = p;
    int left = ph->id, right = (ph->id + 1) % N;
    /* always take the lower-numbered fork first: breaks the circular wait */
    int first = left < right ? left : right;
    int second = left < right ? right : left;
    for (int m = 0; m < MEALS; m++) {
        pthread_mutex_lock(&fork_mu[first]);
        pthread_mutex_lock(&fork_mu[second]);
        if (fork_owner[first] != -1 || fork_owner[second] != -1)
            ph->overlap_bad++;
        fork_owner[first] = ph->id;
        fork_owner[second] = ph->id;
        ph->meals++;
        ph->ate_units += ph->id + 1;
        if (fork_owner[first] != ph->id || fork_owner[second] != ph->id)
            ph->overlap_bad++;
        fork_owner[first] = -1;
        fork_owner[second] = -1;
        pthread_mutex_unlock(&fork_mu[second]);
        pthread_mutex_unlock(&fork_mu[first]);
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < N; i++) {
        pthread_mutex_init(&fork_mu[i], NULL);
        fork_owner[i] = -1;
    }
    Phil ph[N];
    pthread_t th[N];
    for (int i = 0; i < N; i++) {
        ph[i].id = i;
        ph[i].meals = 0;
        ph[i].ate_units = 0;
        ph[i].overlap_bad = 0;
        check(pthread_create(&th[i], NULL, philosopher, &ph[i]) == 0, "create");
    }
    long total = 0;
    for (int i = 0; i < N; i++) {
        pthread_join(th[i], NULL);
        check(ph[i].meals == MEALS, "meals");
        check(ph[i].overlap_bad == 0, "fork exclusivity");
        printf("philosopher %d: meals=%d units=%ld\n", i, ph[i].meals, ph[i].ate_units);
        total += ph[i].meals;
    }
    printf("total meals=%ld\n", total);
    for (int i = 0; i < N; i++) {
        check(fork_owner[i] == -1, "fork free");
        pthread_mutex_destroy(&fork_mu[i]);
    }
    return 0;
}
