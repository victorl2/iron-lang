/*
 * title: Pool shutdown modes: graceful drain versus immediate discard
 * topic: concurrency
 * covers: shutdown state machine, drain accepts child tasks only from workers, shutdown-now discards queue, rejected submits
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Job {
    void (*fn)(struct Job *);
    int depth;
    struct Job *next;
} Job;

typedef enum { P_RUNNING, P_DRAINING, P_STOPPING, P_STOPPED } PState;
typedef enum { SUB_OK, SUB_REJECTED_SHUTDOWN } SubResult;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Job *head, *tail;
    int queued, active, nworkers;
    PState st;
    long executed;
    pthread_t *th;
} Pool;

static pthread_key_t worker_key;
static Pool *cur;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static const char *sub_name(SubResult r) {
    return r == SUB_OK ? "accepted" : "rejected (shutdown)";
}

static SubResult submit(Pool *p, void (*fn)(Job *), int depth) {
    int from_worker = pthread_getspecific(worker_key) != NULL;
    pthread_mutex_lock(&p->mu);
    if (p->st == P_STOPPING || p->st == P_STOPPED || (p->st == P_DRAINING && !from_worker)) {
        pthread_mutex_unlock(&p->mu);
        return SUB_REJECTED_SHUTDOWN;
    }
    Job *j = malloc(sizeof *j);
    check(j != NULL, "alloc");
    j->fn = fn;
    j->depth = depth;
    j->next = NULL;
    if (p->tail)
        p->tail->next = j;
    else
        p->head = j;
    p->tail = j;
    p->queued++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return SUB_OK;
}

static void *worker(void *arg) {
    Pool *p = arg;
    pthread_setspecific(worker_key, p);
    pthread_mutex_lock(&p->mu);
    for (;;) {
        while (p->queued == 0 && !(p->st != P_RUNNING && p->active == 0))
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->queued == 0)
            break;
        Job *j = p->head;
        p->head = j->next;
        if (!p->head)
            p->tail = NULL;
        p->queued--;
        p->active++;
        pthread_mutex_unlock(&p->mu);
        j->fn(j);
        free(j);
        pthread_mutex_lock(&p->mu);
        p->active--;
        p->executed++;
        pthread_cond_broadcast(&p->cv);
    }
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

static void pool_start(Pool *p, int n) {
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->head = p->tail = NULL;
    p->queued = p->active = 0;
    p->executed = 0;
    p->st = P_RUNNING;
    p->nworkers = n;
    p->th = malloc(sizeof(pthread_t) * (size_t)n);
    check(p->th != NULL, "alloc");
    for (int i = 0; i < n; i++)
        check(pthread_create(&p->th[i], NULL, worker, p) == 0, "create");
}

static void pool_finish(Pool *p) {
    for (int i = 0; i < p->nworkers; i++)
        pthread_join(p->th[i], NULL);
    pthread_mutex_lock(&p->mu);
    p->st = P_STOPPED;
    pthread_mutex_unlock(&p->mu);
    free(p->th);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
}

static void shutdown_drain(Pool *p) {
    pthread_mutex_lock(&p->mu);
    p->st = P_DRAINING;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    pool_finish(p);
}

static int shutdown_now(Pool *p) {
    pthread_mutex_lock(&p->mu);
    p->st = P_STOPPING;
    int dropped = 0;
    while (p->head) {
        Job *j = p->head;
        p->head = j->next;
        free(j);
        dropped++;
    }
    p->tail = NULL;
    p->queued = 0;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return dropped;
}

/* Scenario A: recursive tasks that spawn two children up to depth 3. */
static void tree_task(Job *j) {
    if (j->depth < 3) {
        SubResult a = submit(cur, tree_task, j->depth + 1);
        SubResult b = submit(cur, tree_task, j->depth + 1);
        check(a == SUB_OK && b == SUB_OK, "children accepted while draining");
    }
}

/* Scenario B: tasks blocked on a gate so that the queue is full of pending work. */
static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open, gate_waiters;

static void gate_task(Job *j) {
    pthread_mutex_lock(&gate_mu);
    gate_waiters++;
    pthread_cond_broadcast(&gate_cv);
    while (!gate_open)
        pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}

static void noop_task(Job *j) {}

int main(void) {
    pthread_key_create(&worker_key, NULL);
    Pool a;
    cur = &a;
    pool_start(&a, 3);
    for (int i = 0; i < 3; i++)
        check(submit(&a, tree_task, 0) == SUB_OK, "root accepted");
    shutdown_drain(&a);
    SubResult late = submit(&a, noop_task, 0);
    printf("drain: executed %ld tasks (3 trees of 15)\n", a.executed);
    printf("drain: submit after shutdown %s\n", sub_name(late));
    check(a.executed == 45, "drain ran every task including children");
    check(a.queued == 0 && a.active == 0, "drained");

    Pool b;
    cur = &b;
    pool_start(&b, 2);
    check(submit(&b, gate_task, 0) == SUB_OK, "gate 1");
    check(submit(&b, gate_task, 0) == SUB_OK, "gate 2");
    pthread_mutex_lock(&gate_mu);
    while (gate_waiters < 2)
        pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
    for (int i = 0; i < 10; i++)
        check(submit(&b, noop_task, 0) == SUB_OK, "pending accepted");
    int dropped = shutdown_now(&b);
    SubResult late2 = submit(&b, noop_task, 0);
    pthread_mutex_lock(&gate_mu);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
    pool_finish(&b);
    printf("now: discarded %d pending tasks\n", dropped);
    printf("now: executed %ld tasks (the two running ones finish)\n", b.executed);
    printf("now: submit after shutdown %s\n", sub_name(late2));
    check(dropped == 10 && b.executed == 2, "shutdown-now accounting");
    pthread_key_delete(worker_key);
    return 0;
}
