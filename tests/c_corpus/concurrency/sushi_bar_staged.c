/*
 * title: Sushi bar with staged arrivals
 * topic: concurrency
 * covers: sushi bar problem, must-wait flag, group admission when the bar empties, FIFO waiting queue, monitor
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SEATS = 5, MAXC = 24, MAXLOG = 80 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static int eating, must_wait;
static int wait_q[MAXC], wq_head, wq_tail;
static int admitted[MAXC], may_leave[MAXC], left[MAXC], settled[MAXC];
static int settled_n;
static char logbuf[MAXLOG][48];
static int nlog;
static int bad_admit;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void note(const char *fmt, int a, int b) {
    snprintf(logbuf[nlog++], sizeof logbuf[0], fmt, a, b);
}

static void *customer(void *arg) {
    int id = (int)(long)arg;
    pthread_mutex_lock(&mu);
    if (must_wait) {
        wait_q[wq_tail++] = id;
        note("customer %d waits outside (%d in queue)", id, wq_tail - wq_head);
        settled[id] = 1;
        settled_n++;
        pthread_cond_broadcast(&cv);
        while (!admitted[id])
            pthread_cond_wait(&cv, &mu);
        /* eating was already incremented on our behalf by the last leaver */
    } else {
        eating++;
        must_wait = eating == SEATS;
        note("customer %d sits (%d eating)", id, eating);
        admitted[id] = 1;
        settled[id] = 1;
        settled_n++;
        pthread_cond_broadcast(&cv);
    }
    if (eating > SEATS)
        bad_admit++;
    while (!may_leave[id])
        pthread_cond_wait(&cv, &mu);
    eating--;
    if (eating == 0) { /* last one out lets the waiting group in */
        int n = wq_tail - wq_head;
        if (n > SEATS)
            n = SEATS;
        note("bar empty, %d waiting customers admitted", n, 0);
        for (int i = 0; i < n; i++) {
            int w = wait_q[wq_head++];
            admitted[w] = 1;
            eating++;
        }
        must_wait = eating == SEATS;
    }
    left[id] = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static pthread_t th[MAXC];

static void arrive(int id) {
    pthread_mutex_lock(&mu);
    int want = settled_n + 1;
    pthread_mutex_unlock(&mu);
    check(pthread_create(&th[id], NULL, customer, (void *)(long)id) == 0, "create");
    pthread_mutex_lock(&mu);
    while (settled_n < want)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void finish(int id) {
    pthread_mutex_lock(&mu);
    check(admitted[id], "only seated customers finish");
    may_leave[id] = 1;
    pthread_cond_broadcast(&cv);
    while (!left[id])
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

int main(void) {
    for (int i = 0; i < 5; i++)
        arrive(i); /* bar fills up */
    for (int i = 5; i < 9; i++)
        arrive(i); /* four more must wait */
    finish(2);
    finish(0); /* two seats free but nobody may enter until the bar is empty */
    pthread_mutex_lock(&mu);
    check(admitted[5] == 0 && eating == 3, "waiters stay outside while others still eat");
    pthread_mutex_unlock(&mu);
    arrive(9); /* arrives while the bar is not yet empty: must wait too */
    finish(1);
    finish(3);
    finish(4); /* the bar is empty: 5,6,7,8,9 fill it completely */
    pthread_mutex_lock(&mu);
    check(admitted[5] && admitted[6] && admitted[7] && admitted[8] && admitted[9], "group admitted");
    check(eating == SEATS && must_wait, "full again");
    pthread_mutex_unlock(&mu);
    arrive(10);
    arrive(11); /* both have to wait */
    finish(5);
    finish(6);
    pthread_mutex_lock(&mu);
    check(!admitted[10] && !admitted[11] && eating == 3, "still closed while three eat");
    pthread_mutex_unlock(&mu);
    finish(7);
    finish(8);
    finish(9); /* empty again: 10 and 11 enter together, three seats stay free */
    pthread_mutex_lock(&mu);
    check(admitted[10] && admitted[11] && eating == 2 && !must_wait, "second group");
    pthread_mutex_unlock(&mu);
    arrive(12); /* seats are free and nobody is forced to wait: sits at once */
    finish(10);
    finish(11);
    finish(12);
    for (int i = 0; i < 13; i++)
        pthread_join(th[i], NULL);

    for (int i = 0; i < nlog; i++)
        printf("%2d: %s\n", i + 1, logbuf[i]);
    check(eating == 0 && wq_head == wq_tail && !must_wait, "bar empty at the end");
    check(bad_admit == 0, "never more than SEATS");
    return 0;
}
