/*
 * title: Sleeping barber with staged arrivals
 * topic: concurrency
 * covers: sleeping barber, bounded waiting room, FIFO service, monitor, turned-away customers
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAXC = 32 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t barber_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cust_cv = PTHREAD_COND_INITIALIZER;

static int chairs = 3;
static int queue[MAXC], qhead, qtail; /* waiting room, FIFO */
static int shop_open;                 /* barber ignores customers until this is set */
static int quitting;
static int arrived;                   /* customers that finished their arrival decision */
static int done_flag[MAXC];
static int outcome[MAXC]; /* 0 = unknown, 1 = served, 2 = left */
static int served_order[MAXC], served_n;
static int cut_len[MAXC];
static long total_cut_time;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *barber(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!quitting && (!shop_open || qhead == qtail))
            pthread_cond_wait(&barber_cv, &mu); /* asleep */
        if (quitting && qhead == qtail)
            break;
        int c = queue[qhead++];
        served_order[served_n++] = c;
        total_cut_time += cut_len[c];
        done_flag[c] = 1;
        pthread_cond_broadcast(&cust_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void *customer(void *arg) {
    int id = (int)(long)arg;
    pthread_mutex_lock(&mu);
    if (qtail - qhead >= chairs) {
        outcome[id] = 2; /* waiting room full: walk out */
        arrived++;
        pthread_cond_broadcast(&cust_cv);
        pthread_mutex_unlock(&mu);
        return NULL;
    }
    queue[qtail++] = id;
    outcome[id] = 1;
    arrived++;
    pthread_cond_signal(&barber_cv);
    pthread_cond_broadcast(&cust_cv);
    while (!done_flag[id])
        pthread_cond_wait(&cust_cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static unsigned rng_state = 2024u;
static unsigned rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* create customers one at a time so that arrival order is fixed */
static void arrive_batch(pthread_t *th, int first, int count) {
    for (int i = first; i < first + count; i++) {
        cut_len[i] = (int)(rng() % 20) + 10;
        check(pthread_create(&th[i], NULL, customer, (void *)(long)i) == 0, "create");
        pthread_mutex_lock(&mu);
        while (arrived < i + 1)
            pthread_cond_wait(&cust_cv, &mu);
        pthread_mutex_unlock(&mu);
    }
}

int main(void) {
    pthread_t bt, th[MAXC];
    check(pthread_create(&bt, NULL, barber, NULL) == 0, "create barber");

    /* phase 1: the barber has not opened; 8 customers show up, 3 chairs */
    arrive_batch(th, 0, 8);
    pthread_mutex_lock(&mu);
    shop_open = 1;
    pthread_cond_signal(&barber_cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 8; i++)
        pthread_join(th[i], NULL);

    /* phase 2: bigger waiting room, all customers get served in arrival order */
    pthread_mutex_lock(&mu);
    chairs = 12;
    pthread_mutex_unlock(&mu);
    arrive_batch(th, 8, 10);
    for (int i = 8; i < 18; i++)
        pthread_join(th[i], NULL);

    pthread_mutex_lock(&mu);
    quitting = 1;
    pthread_cond_signal(&barber_cv);
    pthread_mutex_unlock(&mu);
    pthread_join(bt, NULL);

    printf("phase 1 outcomes:");
    for (int i = 0; i < 8; i++)
        printf(" %d:%s", i, outcome[i] == 1 ? "served" : "left");
    printf("\nphase 2 outcomes:");
    for (int i = 8; i < 18; i++)
        printf(" %d:%s", i, outcome[i] == 1 ? "served" : "left");
    printf("\nservice order:");
    for (int i = 0; i < served_n; i++)
        printf(" %d", served_order[i]);
    printf("\nserved=%d left=%d total cut time=%ld\n", served_n, 18 - served_n, total_cut_time);

    long expect = 0;
    int exp_n = 0, prev = -1;
    for (int i = 0; i < 18; i++)
        if (outcome[i] == 1) {
            expect += cut_len[i];
            exp_n++;
        }
    check(exp_n == served_n && expect == total_cut_time, "accounting");
    for (int i = 0; i < served_n; i++) {
        check(served_order[i] > prev, "FIFO service");
        prev = served_order[i];
    }
    check(served_n == 13, "expected served count");
    return 0;
}
