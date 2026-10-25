/*
 * title: Fork-join tree reduction with helping joins
 * topic: concurrency
 * covers: fork/join, shared LIFO task stack, join that runs other tasks, struct reduction, sequential cutoff
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { NNODES = 16383, NW = 3, CUTOFF = 200 };

typedef struct {
    long sum, max, count, height;
} Agg;

typedef struct Task {
    int node;
    Agg result;
    int done;
} Task;

static long tree[NNODES];
static Task *stack[256];
static int sp;
static int stop;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Agg combine(Agg l, Agg r, long v) {
    Agg a;
    a.sum = l.sum + r.sum + v;
    a.max = v;
    if (l.max > a.max)
        a.max = l.max;
    if (r.max > a.max)
        a.max = r.max;
    a.count = l.count + r.count + 1;
    a.height = 1 + (l.height > r.height ? l.height : r.height);
    return a;
}

static Agg seq(int n) {
    Agg zero = {0, -1, 0, 0};
    if (n >= NNODES)
        return zero;
    Agg l = seq(2 * n + 1), r = seq(2 * n + 2);
    return combine(l, r, tree[n]);
}

static void run_task(Task *t);

static int try_run_one(void) {
    Task *t = NULL;
    pthread_mutex_lock(&mu);
    if (sp > 0)
        t = stack[--sp];
    pthread_mutex_unlock(&mu);
    if (!t)
        return 0;
    run_task(t);
    return 1;
}

static Agg par(int n);

static void run_task(Task *t) {
    Agg r = par(t->node);
    pthread_mutex_lock(&mu);
    t->result = r;
    t->done = 1;
    pthread_mutex_unlock(&mu);
}

static int is_done(Task *t) {
    pthread_mutex_lock(&mu);
    int d = t->done;
    pthread_mutex_unlock(&mu);
    return d;
}

static Agg par(int n) {
    Agg zero = {0, -1, 0, 0};
    if (n >= NNODES)
        return zero;
    /* subtree size from the node's depth */
    int size = 1, lo = n, hi = n;
    while (2 * lo + 1 < NNODES) {
        lo = 2 * lo + 1;
        hi = 2 * hi + 2;
        size += (hi < NNODES ? hi : NNODES - 1) - lo + 1;
    }
    if (size <= CUTOFF)
        return seq(n);
    Task left = {2 * n + 1, zero, 0};
    pthread_mutex_lock(&mu);
    check(sp < 256, "stack space");
    stack[sp++] = &left;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    Agg r = par(2 * n + 2);
    while (!is_done(&left)) {
        if (!try_run_one())
            sched_yield();
    }
    return combine(left.result, r, tree[n]);
}

static void *worker(void *arg) {
    for (;;) {
        pthread_mutex_lock(&mu);
        while (sp == 0 && !stop)
            pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        if (!try_run_one()) {
            pthread_mutex_lock(&mu);
            int s = stop && sp == 0;
            pthread_mutex_unlock(&mu);
            if (s)
                return NULL;
        }
    }
}

int main(void) {
    unsigned s = 31337u;
    for (int i = 0; i < NNODES; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        tree[i] = (long)(s % 100000u);
    }
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    Agg p = par(0);
    pthread_mutex_lock(&mu);
    stop = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);
    Agg q = seq(0);
    check(p.sum == q.sum && p.max == q.max && p.count == q.count && p.height == q.height, "parallel equals sequential");
    check(p.count == NNODES, "visited every node");
    check(sp == 0, "stack empty");
    printf("nodes %ld height %ld\n", p.count, p.height);
    printf("sum %ld max %ld\n", p.sum, p.max);
    Agg l = par(1), r = par(2);
    printf("left subtree sum %ld right subtree sum %ld\n", l.sum, r.sum);
    check(l.sum + r.sum + tree[0] == p.sum, "subtrees add up");
    return 0;
}
