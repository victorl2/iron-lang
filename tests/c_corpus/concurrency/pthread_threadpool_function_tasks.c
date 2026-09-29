/*
 * title: Fixed thread pool running function-pointer tasks
 * topic: concurrency
 * covers: worker pool, task structs with function pointers, futures via per-task completion, results by index
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WORKERS = 4, TASKS = 40 };

typedef long (*TaskFn)(long);

typedef struct {
    TaskFn fn;
    long arg;
    long result;
    int done;
} Task;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t work_cv, done_cv;
    Task *tasks[TASKS];
    int head, tail;
    int shutdown;
} Pool;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static long fib(long n) {
    long a = 0, b = 1;
    for (long i = 0; i < n; i++) {
        long t = (a + b) % 1000000007L;
        a = b;
        b = t;
    }
    return a;
}

static long tri(long n) {
    return n * (n + 1) / 2;
}

static long popcount_l(long n) {
    long c = 0;
    unsigned long u = (unsigned long)n;
    while (u) {
        c += (long)(u & 1u);
        u >>= 1;
    }
    return c;
}

static long digits_rev(long n) {
    long r = 0;
    while (n > 0) {
        r = r * 10 + n % 10;
        n /= 10;
    }
    return r;
}

static TaskFn fns[4] = {fib, tri, popcount_l, digits_rev};
static const char *fn_names[4] = {"fib", "tri", "popcount", "digits_rev"};

static void *worker(void *p) {
    Pool *pl = p;
    for (;;) {
        pthread_mutex_lock(&pl->mu);
        while (pl->head == pl->tail && !pl->shutdown)
            pthread_cond_wait(&pl->work_cv, &pl->mu);
        if (pl->head == pl->tail) {
            pthread_mutex_unlock(&pl->mu);
            return NULL;
        }
        Task *t = pl->tasks[pl->head++];
        pthread_mutex_unlock(&pl->mu);
        long r = t->fn(t->arg);
        pthread_mutex_lock(&pl->mu);
        t->result = r;
        t->done = 1;
        pthread_cond_broadcast(&pl->done_cv);
        pthread_mutex_unlock(&pl->mu);
    }
}

static void await(Pool *pl, Task *t) {
    pthread_mutex_lock(&pl->mu);
    while (!t->done)
        pthread_cond_wait(&pl->done_cv, &pl->mu);
    pthread_mutex_unlock(&pl->mu);
}

int main(void) {
    Pool pl;
    memset(&pl, 0, sizeof pl);
    pthread_mutex_init(&pl.mu, NULL);
    pthread_cond_init(&pl.work_cv, NULL);
    pthread_cond_init(&pl.done_cv, NULL);
    pthread_t th[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        check(pthread_create(&th[i], NULL, worker, &pl) == 0, "create");

    static Task tasks[TASKS];
    for (int i = 0; i < TASKS; i++) {
        tasks[i].fn = fns[i % 4];
        tasks[i].arg = 10 + i * 3;
        pthread_mutex_lock(&pl.mu);
        pl.tasks[pl.tail++] = &tasks[i];
        pthread_cond_signal(&pl.work_cv);
        pthread_mutex_unlock(&pl.mu);
    }
    long total = 0;
    for (int i = 0; i < TASKS; i++) {
        await(&pl, &tasks[i]);
        check(tasks[i].result == tasks[i].fn(tasks[i].arg), "task result");
        total += tasks[i].result;
        if (i < 8)
            printf("task %2d %-10s(%ld) = %ld\n", i, fn_names[i % 4], tasks[i].arg, tasks[i].result);
    }
    pthread_mutex_lock(&pl.mu);
    pl.shutdown = 1;
    pthread_cond_broadcast(&pl.work_cv);
    pthread_mutex_unlock(&pl.mu);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(th[i], NULL);
    printf("tasks=%d total=%ld\n", TASKS, total);
    return 0;
}
