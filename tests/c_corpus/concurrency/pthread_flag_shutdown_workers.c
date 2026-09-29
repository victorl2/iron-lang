/*
 * title: Cancellation-free shutdown via flags
 * topic: concurrency
 * covers: cooperative shutdown, stop flag under mutex, drain-then-exit, work queue accounting
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { WORKERS = 4, JOBS = 400 };

typedef struct Job {
    int n;
    struct Job *next;
} Job;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Job *head, *tail;
    int stopping; /* set by main; workers finish queued work then exit */
    int pending;
} Queue;

typedef struct {
    Queue *q;
    int id;
    long processed;
    unsigned long long acc;
    int saw_stop;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned long long work(int n) {
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < 50; i++) {
        h ^= (unsigned long long)(n + i);
        h *= 1099511628211ull;
    }
    return h;
}

static void *worker(void *p) {
    Arg *a = p;
    Queue *q = a->q;
    for (;;) {
        pthread_mutex_lock(&q->mu);
        while (!q->head && !q->stopping)
            pthread_cond_wait(&q->cv, &q->mu);
        if (!q->head) { /* stopping and drained */
            a->saw_stop = 1;
            pthread_mutex_unlock(&q->mu);
            return NULL;
        }
        Job *j = q->head;
        q->head = j->next;
        if (!q->head)
            q->tail = NULL;
        q->pending--;
        pthread_mutex_unlock(&q->mu);
        a->acc += work(j->n);
        a->processed++;
        free(j);
    }
}

int main(void) {
    Queue q = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0, 0};
    Arg args[WORKERS];
    pthread_t th[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        args[i].q = &q;
        args[i].id = i;
        args[i].processed = 0;
        args[i].acc = 0;
        args[i].saw_stop = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int n = 0; n < JOBS; n++) {
        Job *j = malloc(sizeof *j);
        check(j != NULL, "malloc");
        j->n = n;
        j->next = NULL;
        pthread_mutex_lock(&q.mu);
        if (q.tail)
            q.tail->next = j;
        else
            q.head = j;
        q.tail = j;
        q.pending++;
        pthread_cond_signal(&q.cv);
        pthread_mutex_unlock(&q.mu);
    }
    /* request shutdown while jobs may still be queued: they must all still run */
    pthread_mutex_lock(&q.mu);
    q.stopping = 1;
    pthread_cond_broadcast(&q.cv);
    pthread_mutex_unlock(&q.mu);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(th[i], NULL);

    long processed = 0;
    unsigned long long acc = 0, expect = 0;
    for (int i = 0; i < WORKERS; i++) {
        check(args[i].saw_stop, "worker observed the stop flag");
        processed += args[i].processed;
        acc += args[i].acc;
    }
    for (int n = 0; n < JOBS; n++)
        expect += work(n);
    check(processed == JOBS, "all jobs processed before exit");
    check(acc == expect, "checksum");
    check(q.pending == 0 && q.head == NULL, "queue empty");
    printf("jobs=%d processed=%ld\n", JOBS, processed);
    printf("checksum=%llu\n", acc);
    printf("all workers saw stop: yes\n");
    return 0;
}
