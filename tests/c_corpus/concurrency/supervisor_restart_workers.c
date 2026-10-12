/*
 * title: Supervisor restarting crashed workers from checkpoints
 * topic: concurrency
 * covers: supervision tree, exit notifications, restart budget, checkpoint resume, poisonous job, escalation to dead
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { NWORKERS = 4, NJOBS = 12, MAX_RESTARTS = 3 };
typedef enum { W_RUNNING, W_DONE, W_CRASHED, W_DEAD } WState;

typedef struct {
    int id;
    pthread_t th;
    WState st;
    int progress;       /* checkpoint: next job index */
    int restarts;
    long sum;
    int processed;
    int crashed_once[NJOBS];
    int crash_count;
} Worker;

static Worker w[NWORKERS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int exited[64], nexited, exited_head;

static void check(int c, const char *w2) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w2);
        exit(1);
    }
}

static long job_value(int worker, int k) {
    return (worker + 1) * 100 + k * 7 + (k * k) % 5;
}

/* Job k of worker i crashes the first time when (k + i) is divisible by 5.
   Worker 2 additionally has a poisonous job (index 5) that crashes on every attempt. */
static int crashes(Worker *x, int k) {
    if (x->id == 2 && k == 5)
        return 1;
    return (k + x->id) % 5 == 0 && !x->crashed_once[k];
}

static void *worker_main(void *arg) {
    Worker *x = arg;
    WState result = W_DONE;
    for (;;) {
        pthread_mutex_lock(&mu);
        int k = x->progress;
        pthread_mutex_unlock(&mu);
        if (k >= NJOBS)
            break;
        if (crashes(x, k)) {
            pthread_mutex_lock(&mu);
            x->crashed_once[k] = 1;
            x->crash_count++;
            pthread_mutex_unlock(&mu);
            result = W_CRASHED;
            break;
        }
        pthread_mutex_lock(&mu);
        x->sum += job_value(x->id, k);
        x->processed++;
        x->progress = k + 1; /* checkpoint after each job */
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    x->st = result;
    exited[nexited++] = x->id;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void start(Worker *x) {
    x->st = W_RUNNING;
    check(pthread_create(&x->th, NULL, worker_main, x) == 0, "create");
}

int main(void) {
    for (int i = 0; i < NWORKERS; i++) {
        w[i].id = i;
        start(&w[i]);
    }
    int live = NWORKERS;
    while (live > 0) {
        pthread_mutex_lock(&mu);
        while (exited_head == nexited)
            pthread_cond_wait(&cv, &mu);
        int id = exited[exited_head++];
        WState st = w[id].st;
        pthread_mutex_unlock(&mu);
        pthread_join(w[id].th, NULL);
        if (st == W_DONE) {
            live--;
        } else if (st == W_CRASHED) {
            if (w[id].restarts < MAX_RESTARTS) {
                w[id].restarts++;
                start(&w[id]); /* one_for_one restart, resumes at the checkpoint */
            } else {
                w[id].st = W_DEAD;
                live--;
            }
        }
    }
    static const char *names[] = {"running", "done", "crashed", "dead"};
    long total = 0;
    int total_restarts = 0, dead = 0;
    for (int i = 0; i < NWORKERS; i++) {
        long expect = 0;
        int upto = w[i].st == W_DEAD ? 5 : NJOBS;
        for (int k = 0; k < upto; k++)
            expect += job_value(i, k);
        check(w[i].sum == expect && w[i].processed == upto, "sum of processed jobs");
        check(w[i].crash_count == w[i].restarts + (w[i].st == W_DEAD ? 1 : 0), "each crash caused one restart or death");
        printf("worker %d: %-4s restarts %d crashes %d processed %2d/%d sum %ld\n", i, names[w[i].st], w[i].restarts,
               w[i].crash_count, w[i].processed, NJOBS, w[i].sum);
        total += w[i].sum;
        total_restarts += w[i].restarts;
        dead += w[i].st == W_DEAD;
    }
    check(dead == 1 && w[2].st == W_DEAD, "only the poisoned worker dies");
    check(nexited == exited_head, "all notifications consumed");
    printf("total restarts %d, dead workers %d, total sum %ld\n", total_restarts, dead, total);
    printf("exit notifications %d\n", nexited);
    return 0;
}
