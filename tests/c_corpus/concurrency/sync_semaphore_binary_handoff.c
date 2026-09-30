/*
 * title: Binary semaphore as mutex and as signal
 * topic: concurrency
 * covers: binary semaphore, mutual exclusion, signalling between threads, strict alternation, lost wakeup check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* Binary semaphore: value is 0 or 1, release on a full semaphore is a counted overflow. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int value;
    int overflows;
} BSem;

static void bs_init(BSem *s, int v) {
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->value = v;
    s->overflows = 0;
}
static void bs_destroy(BSem *s) {
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
}
static void bs_wait(BSem *s) {
    pthread_mutex_lock(&s->mu);
    while (s->value == 0)
        pthread_cond_wait(&s->cv, &s->mu);
    s->value = 0;
    pthread_mutex_unlock(&s->mu);
}
static void bs_post(BSem *s) {
    pthread_mutex_lock(&s->mu);
    if (s->value == 1)
        s->overflows++;
    s->value = 1;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 4, ITERS = 400, TURNS = 500 };

static BSem mutex_sem;
static atomic_int in_cs, violations;
static long counter;

static void *mutex_worker(void *p) {
    (void)p;
    for (int i = 0; i < ITERS; i++) {
        bs_wait(&mutex_sem);
        if (atomic_fetch_add(&in_cs, 1) != 0)
            atomic_fetch_add(&violations, 1);
        long v = counter;
        if (i % 5 == 0)
            sched_yield();
        counter = v + 1;
        atomic_fetch_sub(&in_cs, 1);
        bs_post(&mutex_sem);
    }
    return NULL;
}

/* Two semaphores force strict ping/pong: the "pong" side may only run after "ping" posted. */
static BSem ping_sem, pong_sem;
static int turn_log[2 * TURNS];
static int turn_pos;

static void *ping_thread(void *p) {
    (void)p;
    for (int i = 0; i < TURNS; i++) {
        bs_wait(&ping_sem);
        turn_log[turn_pos++] = 0;
        bs_post(&pong_sem);
    }
    return NULL;
}
static void *pong_thread(void *p) {
    (void)p;
    for (int i = 0; i < TURNS; i++) {
        bs_wait(&pong_sem);
        turn_log[turn_pos++] = 1;
        bs_post(&ping_sem);
    }
    return NULL;
}

int main(void) {
    bs_init(&mutex_sem, 1);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, mutex_worker, NULL);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    printf("mutex mode: counter=%ld expected=%d violations=%d overflows=%d\n", counter, T * ITERS,
           atomic_load(&violations), mutex_sem.overflows);
    check(counter == T * ITERS, "counter");
    check(atomic_load(&violations) == 0, "mutual exclusion");
    check(mutex_sem.overflows == 0, "mutex overflow");
    bs_destroy(&mutex_sem);

    bs_init(&ping_sem, 0);
    bs_init(&pong_sem, 0);
    pthread_t a, b;
    pthread_create(&a, NULL, ping_thread, NULL);
    pthread_create(&b, NULL, pong_thread, NULL);
    bs_post(&ping_sem);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    int alt = 1;
    for (int i = 0; i < 2 * TURNS; i++)
        if (turn_log[i] != i % 2)
            alt = 0;
    printf("signal mode: %d turns logged, strictly alternating=%s\n", turn_pos, alt ? "yes" : "no");
    check(alt && turn_pos == 2 * TURNS, "alternation");
    /* Final state: ping was re-posted by the last pong, pong is empty. */
    printf("final values ping=%d pong=%d\n", ping_sem.value, pong_sem.value);
    check(ping_sem.value == 1 && pong_sem.value == 0, "final values");

    /* Coalescing: two posts without a wait collapse into one and count an overflow. */
    BSem s;
    bs_init(&s, 0);
    bs_post(&s);
    bs_post(&s);
    bs_post(&s);
    printf("after 3 posts: value=%d overflows=%d\n", s.value, s.overflows);
    check(s.value == 1 && s.overflows == 2, "coalescing");
    bs_destroy(&s);
    bs_destroy(&ping_sem);
    bs_destroy(&pong_sem);
    return 0;
}
