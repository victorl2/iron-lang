/*
 * title: Retry queue with exponential backoff on a logical clock
 * topic: concurrency
 * covers: transient failures, requeue with due times, quiescence detection, logical time jumps, give-up limit, ready queue workers
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { NTASK = 14, NW = 3, MAX_ATTEMPTS = 5 };
typedef enum { T_READY, T_WAITING, T_DONE, T_GAVE_UP } TState;

typedef struct {
    int fails;    /* attempts that fail before one succeeds */
    int attempts;
    TState st;
    long due;
    long finished_at;
    long last_backoff;
} Task;

static Task tasks[NTASK];
static int ready[NTASK * 8], rh, rt;
static int running;
static long now;
static long total_attempts;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER, idle_cv = PTHREAD_COND_INITIALIZER;
static int stop;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long backoff(int attempt) {
    return 1L << attempt; /* attempt 1 -> 2, 2 -> 4, ... */
}

static void *worker(void *arg) {
    pthread_mutex_lock(&mu);
    for (;;) {
        while (rh == rt && !stop)
            pthread_cond_wait(&cv, &mu);
        if (rh == rt && stop)
            break;
        int i = ready[rh++];
        Task *t = &tasks[i];
        running++;
        t->attempts++;
        total_attempts++;
        int attempt = t->attempts;
        pthread_mutex_unlock(&mu);
        int ok = attempt > t->fails; /* the simulated operation */
        pthread_mutex_lock(&mu);
        if (ok) {
            t->st = T_DONE;
            t->finished_at = now;
        } else if (attempt >= MAX_ATTEMPTS) {
            t->st = T_GAVE_UP;
            t->finished_at = now;
        } else {
            t->st = T_WAITING;
            t->last_backoff = backoff(attempt);
            t->due = now + t->last_backoff;
        }
        running--;
        pthread_cond_broadcast(&idle_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    unsigned s = 60221u;
    for (int i = 0; i < NTASK; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        tasks[i].fails = (int)(s % 7u); /* 0..6: values >= 5 never succeed within the attempt limit */
        tasks[i].st = T_READY;
        ready[rt++] = i;
    }
    pthread_t th[NW];
    for (int i = 0; i < NW; i++)
        check(pthread_create(&th[i], NULL, worker, NULL) == 0, "create");

    int jumps = 0;
    pthread_mutex_lock(&mu);
    for (;;) {
        pthread_cond_broadcast(&cv);
        while (rh != rt || running > 0)
            pthread_cond_wait(&idle_cv, &mu); /* quiescent: nothing ready and nothing running */
        long next_due = -1;
        int open = 0;
        for (int i = 0; i < NTASK; i++)
            if (tasks[i].st == T_WAITING) {
                open++;
                if (next_due < 0 || tasks[i].due < next_due)
                    next_due = tasks[i].due;
            }
        if (open == 0)
            break;
        check(next_due > now, "time moves forward");
        now = next_due; /* jump the logical clock to the earliest retry */
        jumps++;
        for (int i = 0; i < NTASK; i++)
            if (tasks[i].st == T_WAITING && tasks[i].due <= now) {
                tasks[i].st = T_READY;
                ready[rt++] = i;
            }
    }
    stop = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);

    int done = 0, gave_up = 0;
    long makespan = 0;
    for (int i = 0; i < NTASK; i++) {
        Task *t = &tasks[i];
        long expect_time = 0; /* time of the last attempt: sum of backoffs of failed attempts */
        int expect_attempts = t->fails + 1 > MAX_ATTEMPTS ? MAX_ATTEMPTS : t->fails + 1;
        for (int a = 1; a < expect_attempts; a++)
            expect_time += backoff(a);
        check(t->attempts == expect_attempts, "attempt count");
        check(t->finished_at == expect_time, "completion time is the sum of backoffs");
        check(t->st == T_DONE || t->st == T_GAVE_UP, "terminal state");
        if (t->st == T_DONE)
            done++;
        else
            gave_up++;
        if (t->finished_at > makespan)
            makespan = t->finished_at;
        printf("task %2d: fails %d attempts %d %s at t=%ld\n", i, t->fails, t->attempts,
               t->st == T_DONE ? "done   " : "gave up", t->finished_at);
    }
    printf("done %d, gave up %d, total attempts %ld\n", done, gave_up, total_attempts);
    printf("clock jumps %d, final logical time %ld, makespan %ld\n", jumps, now, makespan);
    return 0;
}
