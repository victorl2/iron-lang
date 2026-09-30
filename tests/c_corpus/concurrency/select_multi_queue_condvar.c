/*
 * title: Select over several queues using one shared condvar
 * topic: concurrency
 * covers: select-like wait, priority by queue index, try-select default case, closed-queue detection, per-queue order
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { NQ = 3, CAPQ = 700, NCONS = 2 };
enum { SEL_NONE = -2, SEL_ALL_CLOSED = -1 };

typedef struct {
    long items[CAPQ];
    int head, tail, closed;
} Chan;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Chan ch[NQ];
} Group;

static Group g = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, {{{0}, 0, 0, 0}}};

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void send_to(int q, long v) {
    pthread_mutex_lock(&g.mu);
    check(g.ch[q].tail < CAPQ, "chan space");
    g.ch[q].items[g.ch[q].tail++] = v;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
}

static void close_q(int q) {
    pthread_mutex_lock(&g.mu);
    g.ch[q].closed = 1;
    pthread_cond_broadcast(&g.cv);
    pthread_mutex_unlock(&g.mu);
}

/* Must hold g.mu. Returns the first ready queue (lowest index), SEL_NONE or SEL_ALL_CLOSED. */
static int poll_locked(void) {
    int closed = 0;
    for (int i = 0; i < NQ; i++) {
        if (g.ch[i].head < g.ch[i].tail)
            return i;
        if (g.ch[i].closed)
            closed++;
    }
    return closed == NQ ? SEL_ALL_CLOSED : SEL_NONE;
}

static int select_recv(long *v, int blocking) {
    pthread_mutex_lock(&g.mu);
    int r = poll_locked();
    while (r == SEL_NONE && blocking) {
        pthread_cond_wait(&g.cv, &g.mu);
        r = poll_locked();
    }
    if (r >= 0)
        *v = g.ch[r].items[g.ch[r].head++];
    pthread_mutex_unlock(&g.mu);
    return r;
}

typedef struct {
    long count[NQ], sum[NQ];
    long last[NQ];
    int order_bad;
} Cons;

static void *producer(void *arg) {
    int q = (int)(long)arg;
    int n = 150 + 100 * q;
    for (int i = 0; i < n; i++)
        send_to(q, q * 10000 + i);
    close_q(q);
    return NULL;
}

static void *consumer(void *arg) {
    Cons *c = arg;
    for (int i = 0; i < NQ; i++)
        c->last[i] = -1;
    long v;
    for (;;) {
        int r = select_recv(&v, 1);
        if (r == SEL_ALL_CLOSED)
            return NULL;
        check(r >= 0 && v / 10000 == r, "value came from the selected queue");
        if (v % 10000 <= c->last[r])
            c->order_bad++;
        c->last[r] = v % 10000;
        c->count[r]++;
        c->sum[r] += v;
    }
}

int main(void) {
    /* Phase A: deterministic priority order, no concurrency. */
    long v;
    check(select_recv(&v, 0) == SEL_NONE, "nothing ready initially (default case)");
    for (int i = 0; i < 3; i++)
        send_to(1, 100 + i);
    for (int i = 0; i < 3; i++)
        send_to(0, 10 + i);
    for (int i = 0; i < 2; i++)
        send_to(2, 200 + i);
    printf("priority picks:");
    for (;;) {
        int r = select_recv(&v, 0);
        if (r < 0)
            break;
        printf(" q%d=%ld", r, v);
    }
    printf("\n");
    check(select_recv(&v, 0) == SEL_NONE, "empty again");
    for (int i = 0; i < NQ; i++)
        g.ch[i].head = g.ch[i].tail = 0;

    /* Phase B: concurrent producers, competing consumers. */
    pthread_t pt[NQ], ct[NCONS];
    Cons cons[NCONS] = {{{0}, {0}, {0}, 0}, {{0}, {0}, {0}, 0}};
    for (int i = 0; i < NCONS; i++)
        check(pthread_create(&ct[i], NULL, consumer, &cons[i]) == 0, "consumer");
    for (long i = 0; i < NQ; i++)
        check(pthread_create(&pt[i], NULL, producer, (void *)i) == 0, "producer");
    for (int i = 0; i < NQ; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < NCONS; i++)
        pthread_join(ct[i], NULL);
    check(select_recv(&v, 1) == SEL_ALL_CLOSED, "all closed after drain");
    for (int q = 0; q < NQ; q++) {
        long n = 0, s = 0, es = 0;
        for (int c = 0; c < NCONS; c++) {
            n += cons[c].count[q];
            s += cons[c].sum[q];
            check(cons[c].order_bad == 0, "per queue FIFO seen by each consumer");
        }
        int en = 150 + 100 * q;
        for (int i = 0; i < en; i++)
            es += q * 10000L + i;
        check(n == en && s == es, "queue totals");
        printf("queue %d: received %ld sum %ld\n", q, n, s);
    }
    printf("select after all closed and drained: closed\n");
    return 0;
}
