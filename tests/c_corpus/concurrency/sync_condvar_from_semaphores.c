/*
 * title: Condition variable built from semaphores
 * topic: concurrency
 * covers: condvar from semaphores, waiter count, signal handshake, broadcast, no stored signals, stolen wakeup check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int v;
} Sem;

static void sem_init_(Sem *s, int v) {
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->v = v;
}
static void sem_destroy_(Sem *s) {
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
}
static void sem_p(Sem *s) {
    pthread_mutex_lock(&s->mu);
    while (s->v == 0)
        pthread_cond_wait(&s->cv, &s->mu);
    s->v--;
    pthread_mutex_unlock(&s->mu);
}
static void sem_v(Sem *s) {
    pthread_mutex_lock(&s->mu);
    s->v++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

/* Birrell-style condition variable: s parks waiters, x guards the count, h is the handshake. */
typedef struct {
    Sem s, x, h;
    int waiters;
} SCond;

static void sc_init(SCond *c) {
    sem_init_(&c->s, 0);
    sem_init_(&c->x, 1);
    sem_init_(&c->h, 0);
    c->waiters = 0;
}
static void sc_destroy(SCond *c) {
    sem_destroy_(&c->s);
    sem_destroy_(&c->x);
    sem_destroy_(&c->h);
}
static void sc_wait(SCond *c, pthread_mutex_t *m) {
    sem_p(&c->x);
    c->waiters++;
    sem_v(&c->x);
    pthread_mutex_unlock(m);
    sem_p(&c->s);
    sem_v(&c->h); /* tell the signaller we really woke */
    pthread_mutex_lock(m);
}
static void sc_signal(SCond *c) {
    sem_p(&c->x);
    if (c->waiters > 0) {
        c->waiters--;
        sem_v(&c->s);
        sem_p(&c->h); /* keeps the signal from being stolen by a thread that starts waiting later */
    }
    sem_v(&c->x);
}
static void sc_broadcast(SCond *c) {
    sem_p(&c->x);
    int n = c->waiters;
    for (int i = 0; i < n; i++)
        sem_v(&c->s);
    for (int i = 0; i < n; i++)
        sem_p(&c->h);
    c->waiters = 0;
    sem_v(&c->x);
}
static int sc_waiters(SCond *c) {
    sem_p(&c->x);
    int n = c->waiters;
    sem_v(&c->x);
    return n;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { CAP = 3, P = 3, C = 3, PER = 150 };

static pthread_mutex_t bm = PTHREAD_MUTEX_INITIALIZER;
static SCond not_full_c, not_empty_c;
static int q[CAP], qh, qn;
static long psum, csum_total;
static int cn_total;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;

static void *producer(void *p) {
    int id = (int)(intptr_t)p;
    for (int i = 0; i < PER; i++) {
        int v = id * 1000 + i + 1;
        pthread_mutex_lock(&bm);
        while (qn == CAP)
            sc_wait(&not_full_c, &bm);
        q[(qh + qn) % CAP] = v;
        qn++;
        sc_signal(&not_empty_c);
        pthread_mutex_unlock(&bm);
    }
    return NULL;
}
static void *consumer(void *p) {
    (void)p;
    long s = 0;
    int n = 0;
    for (int i = 0; i < PER; i++) {
        pthread_mutex_lock(&bm);
        while (qn == 0)
            sc_wait(&not_empty_c, &bm);
        s += q[qh];
        qh = (qh + 1) % CAP;
        qn--;
        n++;
        sc_signal(&not_full_c);
        pthread_mutex_unlock(&bm);
    }
    pthread_mutex_lock(&stat_mu);
    csum_total += s;
    cn_total += n;
    pthread_mutex_unlock(&stat_mu);
    return NULL;
}

static SCond gate_c;
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static int open_gen, passed;
static atomic_int woke;
static void *gate_waiter(void *p) {
    (void)p;
    pthread_mutex_lock(&gm);
    int my = open_gen;
    while (open_gen == my)
        sc_wait(&gate_c, &gm);
    passed++;
    atomic_fetch_add(&woke, 1);
    pthread_mutex_unlock(&gm);
    return NULL;
}

int main(void) {
    sc_init(&not_full_c);
    sc_init(&not_empty_c);
    sc_init(&gate_c);

    /* A signal with nobody waiting is not remembered. */
    sc_signal(&gate_c);
    pthread_t w[5];
    pthread_create(&w[0], NULL, gate_waiter, NULL);
    while (sc_waiters(&gate_c) != 1)
        sched_yield();
    for (int i = 0; i < 1000; i++)
        sched_yield();
    printf("waiter after an earlier stray signal: still blocked = %s\n", atomic_load(&woke) == 0 ? "yes" : "no");
    check(atomic_load(&woke) == 0, "stray signal must not be stored");
    pthread_mutex_lock(&gm);
    open_gen++;
    sc_signal(&gate_c);
    pthread_mutex_unlock(&gm);
    pthread_join(w[0], NULL);
    printf("real signal woke exactly %d waiter\n", atomic_load(&woke));
    check(atomic_load(&woke) == 1, "one wake");

    /* Broadcast wakes all present waiters. */
    for (int i = 1; i < 5; i++)
        pthread_create(&w[i], NULL, gate_waiter, NULL);
    while (sc_waiters(&gate_c) != 4)
        sched_yield();
    pthread_mutex_lock(&gm);
    open_gen++;
    sc_broadcast(&gate_c);
    pthread_mutex_unlock(&gm);
    for (int i = 1; i < 5; i++)
        pthread_join(w[i], NULL);
    printf("broadcast released waiters: total passed %d, waiters left %d\n", passed, gate_c.waiters);
    check(passed == 5 && gate_c.waiters == 0, "broadcast");

    /* Producer-consumer over the semaphore-built condvars. */
    pthread_t pt[P], ct[C];
    for (int i = 0; i < C; i++)
        pthread_create(&ct[i], NULL, consumer, NULL);
    for (int i = 0; i < P; i++)
        pthread_create(&pt[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < P; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < C; i++)
        pthread_join(ct[i], NULL);
    for (int id = 0; id < P; id++)
        for (int i = 0; i < PER; i++)
            psum += id * 1000 + i + 1;
    printf("consumed %d items, sum %ld, produced sum %ld\n", cn_total, csum_total, psum);
    check(cn_total == P * PER && csum_total == psum && qn == 0, "producer-consumer");
    check(not_full_c.waiters == 0 && not_empty_c.waiters == 0, "no leftover waiters");
    sc_destroy(&not_full_c);
    sc_destroy(&not_empty_c);
    sc_destroy(&gate_c);
    return 0;
}
