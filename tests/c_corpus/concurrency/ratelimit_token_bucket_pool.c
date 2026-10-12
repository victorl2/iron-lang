/*
 * title: Rate-limited worker pool with a logical token bucket
 * topic: concurrency
 * covers: token bucket with capacity, refill by logical ticks, workers block without tokens, per-tick throughput
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { NW = 4, NTASKS = 100, CAPACITY = 12, TICKS = 16 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER, done_cv = PTHREAD_COND_INITIALIZER;
static int tokens, next_task, processed, closed;
static long result_sum;
static int taken_in_tick[NTASKS];
static int tick_no;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long work(int i) {
    long v = i + 1;
    for (int k = 0; k < 50; k++)
        v = (v * 31 + k) % 100003;
    return v;
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (!closed && (tokens == 0 || next_task >= NTASKS))
            pthread_cond_wait(&work_cv, &mu);
        if (closed && (tokens == 0 || next_task >= NTASKS))
            break;
        tokens--;
        int t = next_task++;
        taken_in_tick[t] = tick_no;
        pthread_mutex_unlock(&mu);
        long r = work(t);
        pthread_mutex_lock(&mu);
        result_sum += r;
        processed++;
        pthread_cond_broadcast(&done_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    /* refill schedule per tick: a burst first, then idle ticks, then a steady trickle */
    int refill[TICKS] = {5, 0, 0, 30, 3, 3, 3, 3, 0, 20, 2, 2, 2, 2, 25, 25};
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");

    int per_tick[TICKS], bucket_after[TICKS], wasted = 0;
    for (int t = 0; t < TICKS; t++) {
        pthread_mutex_lock(&mu);
        tick_no = t;
        int space = CAPACITY - tokens;
        int add = refill[t] < space ? refill[t] : space;
        wasted += refill[t] - add;
        tokens += add;
        int remaining = NTASKS - next_task;
        int expect_now = tokens < remaining ? tokens : remaining;
        int target = processed + expect_now;
        int before = processed;
        pthread_cond_broadcast(&work_cv);
        while (processed < target)
            pthread_cond_wait(&done_cv, &mu);
        per_tick[t] = processed - before;
        bucket_after[t] = tokens;
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    closed = 1;
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    long expect = 0;
    for (int i = 0; i < processed; i++)
        expect += work(i);
    check(result_sum == expect, "results of executed tasks");
    int total = 0;
    for (int t = 0; t < TICKS; t++) {
        total += per_tick[t];
        check(per_tick[t] <= CAPACITY, "per tick throughput bounded by bucket capacity");
    }
    check(total == processed, "accounting");
    for (int i = 0; i < processed; i++)
        check(taken_in_tick[i] >= 0 && taken_in_tick[i] < TICKS, "tick recorded");
    for (int t = 0; t < TICKS; t++)
        printf("tick %2d refill %2d processed %2d bucket left %2d\n", t, refill[t], per_tick[t], bucket_after[t]);
    printf("processed %d of %d tasks, tokens wasted at capacity %d\n", processed, NTASKS, wasted);
    printf("result sum %ld\n", result_sum);
    return 0;
}
