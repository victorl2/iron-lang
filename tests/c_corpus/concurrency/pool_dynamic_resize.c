/*
 * title: Thread pool that grows and shrinks at runtime
 * topic: concurrency
 * covers: dynamic worker count, cooperative retirement, retired-slot joining, live-count handshake
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { MAXW = 8, MAXQ = 4096 };
enum { FREE = 0, RUNNING = 1, RETIRED = 2 };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER, state_cv = PTHREAD_COND_INITIALIZER,
                      idle_cv = PTHREAD_COND_INITIALIZER;
static pthread_t threads[MAXW];
static int state[MAXW];
static int live, target;
static long queue[MAXQ];
static int qhead, qtail, active;
static long total_sum;
static long executed;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void *worker(void *arg) {
    int slot = (int)(long)arg;
    pthread_mutex_lock(&mu);
    for (;;) {
        while (qhead == qtail && live <= target)
            pthread_cond_wait(&work_cv, &mu);
        if (live > target && qhead == qtail) {
            /* surplus worker: retire */
            state[slot] = RETIRED;
            live--;
            pthread_cond_broadcast(&state_cv);
            break;
        }
        long v = queue[qhead++];
        active++;
        pthread_mutex_unlock(&mu);
        long r = v * v % 1013;
        pthread_mutex_lock(&mu);
        total_sum += r;
        executed++;
        active--;
        if (qhead == qtail && active == 0)
            pthread_cond_broadcast(&idle_cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void reap_locked(void) {
    for (int i = 0; i < MAXW; i++)
        if (state[i] == RETIRED) {
            pthread_mutex_unlock(&mu);
            pthread_join(threads[i], NULL);
            pthread_mutex_lock(&mu);
            state[i] = FREE;
        }
}

static void resize(int n) {
    pthread_mutex_lock(&mu);
    target = n;
    while (live < target) {
        int slot = -1;
        for (int i = 0; i < MAXW; i++)
            if (state[i] == FREE) {
                slot = i;
                break;
            }
        check(slot >= 0, "free slot");
        state[slot] = RUNNING;
        live++;
        check(pthread_create(&threads[slot], NULL, worker, (void *)(long)slot) == 0, "create");
    }
    pthread_cond_broadcast(&work_cv);
    while (live > target)
        pthread_cond_wait(&state_cv, &mu);
    reap_locked();
    pthread_mutex_unlock(&mu);
}

static void submit_batch(int n, long base) {
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) {
        check(qtail < MAXQ, "queue space");
        queue[qtail++] = base + i;
    }
    pthread_cond_broadcast(&work_cv);
    pthread_mutex_unlock(&mu);
}

static void wait_idle(void) {
    pthread_mutex_lock(&mu);
    while (qhead != qtail || active != 0)
        pthread_cond_wait(&idle_cv, &mu);
    pthread_mutex_unlock(&mu);
}

int main(void) {
    int plan[] = {2, 6, 3, 1, 5, 0};
    long base = 1, expect = 0;
    int nplan = (int)(sizeof plan / sizeof plan[0]);
    for (int p = 0; p < nplan; p++) {
        int want = plan[p];
        if (want > 0) {
            resize(want);
            pthread_mutex_lock(&mu);
            int lv = live;
            pthread_mutex_unlock(&mu);
            check(lv == want, "live count matches target");
            submit_batch(200, base);
            wait_idle();
            for (int i = 0; i < 200; i++)
                expect += (base + i) * (base + i) % 1013;
            base += 200;
            printf("resize to %d: live %d, executed %ld, sum %ld\n", want, lv, executed, total_sum);
        } else {
            resize(0);
            pthread_mutex_lock(&mu);
            int lv = live;
            pthread_mutex_unlock(&mu);
            printf("resize to 0: live %d\n", lv);
        }
        check(total_sum == expect, "sum after batch");
    }
    for (int i = 0; i < MAXW; i++)
        check(state[i] == FREE, "all slots reaped");
    check(executed == 1000, "executed count");
    printf("final executed %ld sum %ld\n", executed, total_sum);
    return 0;
}
