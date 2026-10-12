/*
 * title: Weighted counting semaphore bounding in-flight work
 * topic: concurrency
 * covers: weighted semaphore built from mutex and condvar, try_acquire, usage watermark, capacity invariant
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

enum { CAPACITY = 4, NT = 8, ITERS = 60 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int avail;
} Sem;

static Sem sem = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, CAPACITY};
static int in_use, max_in_use, over_capacity;
static long done_by_weight[CAPACITY + 1];
static long work_sum;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void sem_acquire(Sem *s, int n) {
    pthread_mutex_lock(&s->mu);
    while (s->avail < n)
        pthread_cond_wait(&s->cv, &s->mu);
    s->avail -= n;
    pthread_mutex_unlock(&s->mu);
}

static int sem_try_acquire(Sem *s, int n) {
    pthread_mutex_lock(&s->mu);
    int ok = s->avail >= n;
    if (ok)
        s->avail -= n;
    pthread_mutex_unlock(&s->mu);
    return ok;
}

static void sem_release(Sem *s, int n) {
    pthread_mutex_lock(&s->mu);
    s->avail += n;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static long job(int id, int i) {
    long v = id * 1000 + i;
    for (int k = 0; k < 20; k++)
        v = (v * 33 + k) % 65521;
    return v;
}

static void *worker(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < ITERS; i++) {
        int w = 1 + (id + i * 3) % CAPACITY; /* weights 1..4 */
        sem_acquire(&sem, w);
        pthread_mutex_lock(&stat_mu);
        in_use += w;
        if (in_use > max_in_use)
            max_in_use = in_use;
        if (in_use > CAPACITY)
            over_capacity++;
        pthread_mutex_unlock(&stat_mu);
        long r = job(id, i);
        sched_yield();
        pthread_mutex_lock(&stat_mu);
        in_use -= w;
        done_by_weight[w]++;
        work_sum += r;
        pthread_mutex_unlock(&stat_mu);
        sem_release(&sem, w);
    }
    return NULL;
}

int main(void) {
    /* deterministic single-thread behaviour first */
    int a = sem_try_acquire(&sem, 3);
    int b = sem_try_acquire(&sem, 2);
    int c = sem_try_acquire(&sem, 1);
    int d = sem_try_acquire(&sem, 1);
    printf("try_acquire 3,2,1,1 on capacity %d: %d %d %d %d\n", CAPACITY, a, b, c, d);
    check(a && !b && c && !d, "single thread try_acquire results");
    sem_release(&sem, 4);
    check(sem.avail == CAPACITY, "restored");

    pthread_t th[NT];
    for (long i = 0; i < NT; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)i) == 0, "create");
    for (int i = 0; i < NT; i++)
        pthread_join(th[i], NULL);

    long expect = 0, ew[CAPACITY + 1] = {0};
    for (int id = 0; id < NT; id++)
        for (int i = 0; i < ITERS; i++) {
            expect += job(id, i);
            ew[1 + (id + i * 3) % CAPACITY]++;
        }
    check(work_sum == expect, "work sum");
    check(over_capacity == 0 && max_in_use <= CAPACITY && max_in_use >= 1, "never above capacity");
    check(in_use == 0 && sem.avail == CAPACITY, "all units returned");
    for (int w = 1; w <= CAPACITY; w++) {
        check(done_by_weight[w] == ew[w], "per weight counts");
        printf("weight %d: %ld jobs\n", w, done_by_weight[w]);
    }
    printf("total jobs %d, work sum %ld\n", NT * ITERS, work_sum);
    printf("in-use never exceeded capacity %d: yes\n", CAPACITY);
    return 0;
}
