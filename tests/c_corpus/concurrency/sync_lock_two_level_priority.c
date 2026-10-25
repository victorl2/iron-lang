/*
 * title: Two-level priority lock with FIFO within each level
 * topic: concurrency
 * covers: priority lock, high and low queues, no preemption of holder, per-waiter condvars, grant order demo
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct W {
    pthread_cond_t cv;
    int granted;
    int id;
    struct W *next;
} W;

typedef struct {
    pthread_mutex_t mu;
    int held;
    W *qh[2], *qt[2]; /* level 1 = high, level 0 = low */
    int qlen[2];
    int low_grants_with_high_waiting;
} PLock;

static PLock pl = {PTHREAD_MUTEX_INITIALIZER, 0, {NULL, NULL}, {NULL, NULL}, {0, 0}, 0};

static void pl_lock(int level, int id) {
    W me;
    pthread_cond_init(&me.cv, NULL);
    me.granted = 0;
    me.id = id;
    me.next = NULL;
    pthread_mutex_lock(&pl.mu);
    if (!pl.held && pl.qlen[0] == 0 && pl.qlen[1] == 0) {
        pl.held = 1;
    } else {
        if (pl.qt[level])
            pl.qt[level]->next = &me;
        else
            pl.qh[level] = &me;
        pl.qt[level] = &me;
        pl.qlen[level]++;
        while (!me.granted)
            pthread_cond_wait(&me.cv, &pl.mu);
    }
    pthread_mutex_unlock(&pl.mu);
    pthread_cond_destroy(&me.cv);
}

static void pl_unlock(void) {
    pthread_mutex_lock(&pl.mu);
    int lvl = pl.qlen[1] > 0 ? 1 : (pl.qlen[0] > 0 ? 0 : -1);
    if (lvl < 0) {
        pl.held = 0;
    } else {
        if (lvl == 0 && pl.qlen[1] > 0)
            pl.low_grants_with_high_waiting++; /* would be a priority inversion; must stay zero */
        W *w = pl.qh[lvl];
        pl.qh[lvl] = w->next;
        if (!pl.qh[lvl])
            pl.qt[lvl] = NULL;
        pl.qlen[lvl]--;
        w->granted = 1;
        pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&pl.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    int level, id;
} Req;

static int order[16], order_len;
static void *ordered(void *p) {
    Req *r = p;
    pl_lock(r->level, r->id);
    order[order_len++] = r->id;
    pl_unlock();
    return NULL;
}

static void wait_queue(int low, int high) {
    for (;;) {
        pthread_mutex_lock(&pl.mu);
        int ok = pl.qlen[0] == low && pl.qlen[1] == high;
        pthread_mutex_unlock(&pl.mu);
        if (ok)
            return;
        sched_yield();
    }
}

enum { HT = 2, LT = 3, PER = 200 };
static atomic_int in_cs, violations;
static long counter;
static int done_by_level[2];

static void *stress(void *p) {
    Req *r = p;
    for (int i = 0; i < PER; i++) {
        pl_lock(r->level, r->id);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (i % 5 == 0)
            sched_yield();
        counter = v + 1;
        done_by_level[r->level]++;
        atomic_fetch_sub(&in_cs, 1);
        pl_unlock();
    }
    return NULL;
}

int main(void) {
    /* Demo: main holds the lock; arrivals L0 H1 L2 H3 L4 queue up one at a time. */
    pl_lock(0, 99);
    Req reqs[5] = {{0, 0}, {1, 1}, {0, 2}, {1, 3}, {0, 4}};
    pthread_t th[5];
    int lows = 0, highs = 0;
    for (int i = 0; i < 5; i++) {
        pthread_create(&th[i], NULL, ordered, &reqs[i]);
        if (reqs[i].level)
            highs++;
        else
            lows++;
        wait_queue(lows, highs);
    }
    pl_unlock();
    for (int i = 0; i < 5; i++)
        pthread_join(th[i], NULL);
    printf("arrival order: L0 H1 L2 H3 L4\ngrant order:  ");
    for (int i = 0; i < order_len; i++)
        printf(" %s%d", reqs[order[i]].level ? "H" : "L", order[i]);
    printf("\n");
    static const int want[5] = {1, 3, 0, 2, 4};
    for (int i = 0; i < 5; i++)
        check(order[i] == want[i], "priority order");

    /* A high-priority arrival does not preempt the current holder. */
    pl_lock(0, 50);
    Req hi = {1, 7};
    order_len = 0;
    pthread_t ht;
    pthread_create(&ht, NULL, ordered, &hi);
    wait_queue(0, 1);
    printf("holder still holds while high waiter queued: %s, order log length %d\n", pl.held ? "yes" : "no",
           order_len);
    check(pl.held && order_len == 0, "no preemption");
    pl_unlock();
    pthread_join(ht, NULL);

    /* Stress with mixed levels. */
    Req sreq[HT + LT];
    pthread_t st[HT + LT];
    for (int i = 0; i < HT + LT; i++) {
        sreq[i].level = i < HT;
        sreq[i].id = i;
        pthread_create(&st[i], NULL, stress, &sreq[i]);
    }
    for (int i = 0; i < HT + LT; i++)
        pthread_join(st[i], NULL);
    printf("stress: high sections %d, low sections %d, counter %ld\n", done_by_level[1], done_by_level[0], counter);
    printf("priority inversions %d, exclusion violations %d\n", pl.low_grants_with_high_waiting,
           atomic_load(&violations));
    check(done_by_level[1] == HT * PER && done_by_level[0] == LT * PER, "sections");
    check(counter == (HT + LT) * PER && atomic_load(&violations) == 0, "counter");
    check(pl.low_grants_with_high_waiting == 0, "inversion");
    check(!pl.held && pl.qlen[0] == 0 && pl.qlen[1] == 0, "idle");
    return 0;
}
