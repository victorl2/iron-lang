/*
 * title: Thread pool returning futures with error status
 * topic: concurrency
 * covers: future objects, blocking get, ready polling, error propagation, wait-all, pool shutdown
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum { ST_PENDING, ST_OK, ST_ERR } FState;

typedef struct Future {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    FState st;
    long value;
    const char *err;
} Future;

typedef struct Job {
    long (*fn)(long, const char **);
    long arg;
    Future *fut;
    struct Job *next;
} Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Job *head, *tail;
static int stop;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void fut_resolve(Future *f, FState st, long v, const char *err) {
    pthread_mutex_lock(&f->mu);
    f->st = st;
    f->value = v;
    f->err = err;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
}

static FState fut_get(Future *f, long *v, const char **err) {
    pthread_mutex_lock(&f->mu);
    while (f->st == ST_PENDING)
        pthread_cond_wait(&f->cv, &f->mu);
    FState st = f->st;
    *v = f->value;
    *err = f->err;
    pthread_mutex_unlock(&f->mu);
    return st;
}

static int fut_ready(Future *f) {
    pthread_mutex_lock(&f->mu);
    int r = f->st != ST_PENDING;
    pthread_mutex_unlock(&f->mu);
    return r;
}

static void *worker(void *arg) {
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!head && !stop)
            pthread_cond_wait(&cv, &mu);
        if (!head) {
            pthread_mutex_unlock(&mu);
            return NULL;
        }
        Job *j = head;
        head = j->next;
        if (!head)
            tail = NULL;
        pthread_mutex_unlock(&mu);
        const char *err = NULL;
        long r = j->fn(j->arg, &err);
        fut_resolve(j->fut, err ? ST_ERR : ST_OK, r, err);
        free(j);
    }
}

static Future *submit(long (*fn)(long, const char **), long arg) {
    Future *f = calloc(1, sizeof *f);
    Job *j = calloc(1, sizeof *j);
    check(f && j, "alloc");
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
    f->st = ST_PENDING;
    j->fn = fn;
    j->arg = arg;
    j->fut = f;
    pthread_mutex_lock(&mu);
    if (tail)
        tail->next = j;
    else
        head = j;
    tail = j;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    return f;
}

static long fib(long n, const char **err) {
    if (n < 0) {
        *err = "negative input";
        return 0;
    }
    long a = 0, b = 1;
    for (long i = 0; i < n; i++) {
        long t = a + b;
        a = b;
        b = t;
    }
    return a;
}

static long safe_div(long n, const char **err) {
    long d = n % 5;
    if (d == 0) {
        *err = "division by zero";
        return 0;
    }
    return 1000 / d;
}

static long isqrt_l(long n, const char **err) {
    if (n > 1000000) {
        *err = "too large";
        return 0;
    }
    long r = 0;
    while ((r + 1) * (r + 1) <= n)
        r++;
    return r;
}

int main(void) {
    enum { NW = 3, NF = 24 };
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");
    Future *fs[NF];
    const char *kinds[3] = {"fib", "div", "isqrt"};
    long (*fns[3])(long, const char **) = {fib, safe_div, isqrt_l};
    long args[NF];
    for (int i = 0; i < NF; i++) {
        args[i] = (i % 3 == 0) ? (i % 6 == 0 ? -1 : 10 + i) : (i % 3 == 1 ? i : (i * 37000L));
        fs[i] = submit(fns[i % 3], args[i]);
    }
    int oks = 0, errs = 0;
    long sum = 0;
    for (int i = 0; i < NF; i++) {
        long v;
        const char *err;
        FState st = fut_get(fs[i], &v, &err);
        check(fut_ready(fs[i]), "ready after get");
        const char *e2 = NULL;
        long expect = fns[i % 3](args[i], &e2);
        check((st == ST_ERR) == (e2 != NULL), "error status matches");
        check(v == expect, "value matches");
        if (st == ST_OK) {
            oks++;
            sum += v;
            printf("%2d %-5s(%ld) -> %ld\n", i, kinds[i % 3], args[i], v);
        } else {
            errs++;
            printf("%2d %-5s(%ld) -> error: %s\n", i, kinds[i % 3], args[i], err);
        }
    }
    pthread_mutex_lock(&mu);
    stop = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < NF; i++) {
        pthread_mutex_destroy(&fs[i]->mu);
        pthread_cond_destroy(&fs[i]->cv);
        free(fs[i]);
    }
    printf("ok %d error %d sum %ld\n", oks, errs, sum);
    return 0;
}
