/*
 * title: Blocking priority queue with total-order keys
 * topic: concurrency
 * covers: binary heap under mutex, tie-breaking total order, concurrent pops monotone per consumer, close
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int prio; /* lower value runs first */
    int producer;
    int idx;
} Task;

static int less(const Task *a, const Task *b) {
    if (a->prio != b->prio)
        return a->prio < b->prio;
    if (a->producer != b->producer)
        return a->producer < b->producer;
    return a->idx < b->idx;
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv, drained;
    Task *h;
    int n, cap, closed;
} PQ;

static PQ pq = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0, 0};

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void pq_push_locked(Task t) {
    if (pq.n == pq.cap) {
        pq.cap = pq.cap ? pq.cap * 2 : 16;
        pq.h = realloc(pq.h, sizeof(Task) * (size_t)pq.cap);
        check(pq.h != NULL, "realloc");
    }
    int i = pq.n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!less(&t, &pq.h[p]))
            break;
        pq.h[i] = pq.h[p];
        i = p;
    }
    pq.h[i] = t;
}

static Task pq_pop_locked(void) {
    Task top = pq.h[0];
    Task last = pq.h[--pq.n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        Task best = last;
        if (l < pq.n && less(&pq.h[l], &best)) {
            m = l;
            best = pq.h[l];
        }
        if (r < pq.n && less(&pq.h[r], &best)) {
            m = r;
            best = pq.h[r];
        }
        if (m == i)
            break;
        pq.h[i] = best;
        i = m;
    }
    if (pq.n > 0)
        pq.h[i] = last;
    return top;
}

static void push(Task t) {
    pthread_mutex_lock(&pq.mu);
    pq_push_locked(t);
    pthread_cond_signal(&pq.cv);
    pthread_mutex_unlock(&pq.mu);
}

static int pop(Task *out) {
    pthread_mutex_lock(&pq.mu);
    while (pq.n == 0 && !pq.closed)
        pthread_cond_wait(&pq.cv, &pq.mu);
    if (pq.n == 0) {
        pthread_mutex_unlock(&pq.mu);
        return 0;
    }
    *out = pq_pop_locked();
    if (pq.n == 0)
        pthread_cond_broadcast(&pq.drained);
    pthread_mutex_unlock(&pq.mu);
    return 1;
}

enum { NP = 4, PER = 250, NC = 3 };
static int prios[NP][PER];

static void *producer(void *p) {
    int id = (int)(long)p;
    for (int i = 0; i < PER; i++) {
        Task t = {prios[id][i], id, i};
        push(t);
    }
    return NULL;
}

typedef struct {
    Task got[NP * PER];
    int n;
} Bag;

static void *consumer(void *p) {
    Bag *b = p;
    Task t;
    while (pop(&t))
        b->got[b->n++] = t;
    return NULL;
}

int main(void) {
    unsigned s = 424242u;
    for (int p = 0; p < NP; p++)
        for (int i = 0; i < PER; i++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            prios[p][i] = (int)(s % 50u);
        }

    /* Phase 1: producers only, then a single consumer sees a globally sorted stream. */
    pthread_t pt[NP];
    for (long i = 0; i < NP; i++)
        check(pthread_create(&pt[i], NULL, producer, (void *)i) == 0, "create");
    for (int i = 0; i < NP; i++)
        pthread_join(pt[i], NULL);
    check(pq.n == NP * PER, "all pushed");

    /* Phase 2: three consumers drain the rest of the heap concurrently. */
    static Bag bags[NC];
    pthread_t ct[NC];
    static Task first[NP * PER];
    int nfirst = 0;
    Task t;
    for (int i = 0; i < 100; i++) {
        check(pop(&t), "pop");
        first[nfirst++] = t;
    }
    for (int i = 1; i < nfirst; i++)
        check(!less(&first[i], &first[i - 1]), "single consumer sorted");
    printf("first 8 of sorted stream (prio:producer:idx)\n");
    for (int i = 0; i < 8; i++)
        printf("  %d:%d:%d\n", first[i].prio, first[i].producer, first[i].idx);

    for (long i = 0; i < NC; i++)
        check(pthread_create(&ct[i], NULL, consumer, &bags[i]) == 0, "create consumer");
    pthread_mutex_lock(&pq.mu);
    while (pq.n > 0)
        pthread_cond_wait(&pq.drained, &pq.mu);
    pq.closed = 1;
    pthread_cond_broadcast(&pq.cv);
    pthread_mutex_unlock(&pq.mu);
    for (int i = 0; i < NC; i++)
        pthread_join(ct[i], NULL);

    int total = nfirst;
    long chk = 0;
    for (int i = 0; i < nfirst; i++)
        chk += first[i].prio * 7L + first[i].producer * 3L + first[i].idx;
    int maxprio_first = first[nfirst - 1].prio;
    int min_rest = 1 << 30;
    for (int c = 0; c < NC; c++) {
        for (int i = 1; i < bags[c].n; i++)
            check(!less(&bags[c].got[i], &bags[c].got[i - 1]), "consumer stream monotone");
        for (int i = 0; i < bags[c].n; i++) {
            total++;
            chk += bags[c].got[i].prio * 7L + bags[c].got[i].producer * 3L + bags[c].got[i].idx;
            if (bags[c].got[i].prio < min_rest)
                min_rest = bags[c].got[i].prio;
        }
    }
    check(total == NP * PER, "total popped");
    check(min_rest >= first[nfirst - 1].prio, "rest not smaller than first hundred");
    long expect = 0;
    for (int p = 0; p < NP; p++)
        for (int i = 0; i < PER; i++)
            expect += prios[p][i] * 7L + p * 3L + i;
    check(chk == expect, "checksum");
    printf("prio of 100th item %d\n", maxprio_first);
    printf("total %d checksum %ld\n", total, chk);
    printf("each consumer stream monotone: yes\n");
    free(pq.h);
    return 0;
}
