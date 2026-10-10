/*
 * title: Work-stealing deques guarded by mutexes
 * topic: concurrency
 * covers: per-worker deque, owner LIFO pop, thief FIFO steal, recursive task spawning, termination by pending count
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    long lo, hi;
} Range;

typedef struct {
    pthread_mutex_t mu;
    Range *buf;
    int cap, top, bot; /* live items are buf[top..bot) */
} Deque;

enum { NW = 4 };
static Deque dq[NW];
static atomic_long pending;
static long partial[NW], leaves[NW], steals[NW], splits[NW];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void dq_init(Deque *d) {
    pthread_mutex_init(&d->mu, NULL);
    d->cap = 64;
    d->buf = malloc(sizeof(Range) * (size_t)d->cap);
    check(d->buf != NULL, "alloc");
    d->top = d->bot = 0;
}

static void dq_push(Deque *d, Range r) {
    pthread_mutex_lock(&d->mu);
    if (d->bot == d->cap) {
        int n = d->bot - d->top;
        if (d->top > 0) { /* compact */
            for (int i = 0; i < n; i++)
                d->buf[i] = d->buf[d->top + i];
        } else {
            d->cap *= 2;
            d->buf = realloc(d->buf, sizeof(Range) * (size_t)d->cap);
            check(d->buf != NULL, "realloc");
        }
        d->top = 0;
        d->bot = n;
    }
    d->buf[d->bot++] = r;
    pthread_mutex_unlock(&d->mu);
}

static int dq_pop(Deque *d, Range *r) { /* owner end */
    int ok = 0;
    pthread_mutex_lock(&d->mu);
    if (d->bot > d->top) {
        *r = d->buf[--d->bot];
        ok = 1;
    }
    pthread_mutex_unlock(&d->mu);
    return ok;
}

static int dq_steal(Deque *d, Range *r) { /* thief end */
    int ok = 0;
    pthread_mutex_lock(&d->mu);
    if (d->bot > d->top) {
        *r = d->buf[d->top++];
        ok = 1;
    }
    pthread_mutex_unlock(&d->mu);
    return ok;
}

static long f(long i) {
    return (i * i + 3 * i) % 97;
}

static void *worker(void *arg) {
    int me = (int)(long)arg;
    for (;;) {
        Range r;
        int got = dq_pop(&dq[me], &r);
        if (!got) {
            for (int k = 1; k < NW && !got; k++)
                if (dq_steal(&dq[(me + k) % NW], &r)) {
                    got = 1;
                    steals[me]++;
                }
        }
        if (!got) {
            if (atomic_load(&pending) == 0)
                return NULL;
            sched_yield();
            continue;
        }
        if (r.hi - r.lo > 40) {
            long mid = r.lo + (r.hi - r.lo) / 2;
            Range a = {r.lo, mid}, b = {mid, r.hi};
            atomic_fetch_add(&pending, 2);
            dq_push(&dq[me], a);
            dq_push(&dq[me], b);
            splits[me]++;
        } else {
            long s = 0;
            for (long i = r.lo; i < r.hi; i++)
                s += f(i);
            partial[me] += s;
            leaves[me]++;
        }
        atomic_fetch_sub(&pending, 1);
    }
}

int main(void) {
    enum { N = 20000 };
    for (int i = 0; i < NW; i++)
        dq_init(&dq[i]);
    Range whole = {0, N};
    atomic_store(&pending, 1);
    dq_push(&dq[0], whole);
    pthread_t th[NW];
    for (long i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)i) == 0, "create");
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    long total = 0, nleaves = 0, nsplits = 0;
    for (int i = 0; i < NW; i++) {
        total += partial[i];
        nleaves += leaves[i];
        nsplits += splits[i];
    }
    long expect = 0;
    for (long i = 0; i < N; i++)
        expect += f(i);
    check(total == expect, "sum");
    check(nleaves == nsplits + 1, "binary tree of tasks: leaves = splits + 1");
    check(atomic_load(&pending) == 0, "no pending");
    for (int i = 0; i < NW; i++) {
        check(dq[i].bot == dq[i].top, "deque empty");
        free(dq[i].buf);
        pthread_mutex_destroy(&dq[i].mu);
    }
    printf("range %d sum %ld\n", N, total);
    printf("leaf tasks %ld split tasks %ld\n", nleaves, nsplits);
    printf("tasks executed %ld\n", nleaves + nsplits);
    return 0;
}
