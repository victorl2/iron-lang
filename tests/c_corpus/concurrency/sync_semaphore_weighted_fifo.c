/*
 * title: Weighted FIFO semaphore without barging
 * topic: concurrency
 * covers: weighted semaphore, acquire n permits, strict FIFO head-of-line blocking, capacity bound, grant order log
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
    int capacity, avail;
    unsigned next_ticket, head_ticket;
    int max_in_use;
    unsigned last_granted;
    int grant_order_bad;
    int first_grant;
} WSem;

static WSem ws = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0, 0, 0, 1};

static void ws_setup(int cap) {
    ws.capacity = ws.avail = cap;
}

/* Strict FIFO: only the head of the ticket queue may take permits, even if a later, smaller request would fit. */
static unsigned ws_acquire(int n) {
    pthread_mutex_lock(&ws.mu);
    unsigned my = ws.next_ticket++;
    while (ws.head_ticket != my || ws.avail < n)
        pthread_cond_wait(&ws.cv, &ws.mu);
    ws.avail -= n;
    ws.head_ticket++;
    if (!ws.first_grant && my != ws.last_granted + 1)
        ws.grant_order_bad++;
    ws.first_grant = 0;
    ws.last_granted = my;
    int in_use = ws.capacity - ws.avail;
    if (in_use > ws.max_in_use)
        ws.max_in_use = in_use;
    pthread_cond_broadcast(&ws.cv);
    pthread_mutex_unlock(&ws.mu);
    return my;
}
static void ws_release(int n) {
    pthread_mutex_lock(&ws.mu);
    ws.avail += n;
    pthread_cond_broadcast(&ws.cv);
    pthread_mutex_unlock(&ws.mu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int grant_seq[8];
static atomic_int grant_pos;
typedef struct {
    int id, weight;
} Req;

static void *demo_req(void *p) {
    Req *r = p;
    ws_acquire(r->weight);
    grant_seq[atomic_fetch_add(&grant_pos, 1)] = r->id;
    return NULL; /* keeps holding: main releases for it */
}

static void wait_tickets(unsigned n) {
    for (;;) {
        pthread_mutex_lock(&ws.mu);
        unsigned t = ws.next_ticket;
        pthread_mutex_unlock(&ws.mu);
        if (t == n)
            return;
        sched_yield();
    }
}

enum { T = 5, PER = 150, CAP = 6 };
static atomic_int in_use_now, over_cap;
static long weight_total;
static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;

static void *stress(void *p) {
    int id = (int)(intptr_t)p;
    unsigned s = 5u + (unsigned)id * 977u;
    long mine = 0;
    for (int i = 0; i < PER; i++) {
        s = s * 1664525u + 1013904223u;
        int w = 1 + (int)((s >> 20) % 4);
        ws_acquire(w);
        int now = atomic_fetch_add(&in_use_now, w) + w;
        if (now > CAP)
            atomic_fetch_add(&over_cap, 1);
        mine += w;
        if (i % 4 == 0)
            sched_yield();
        atomic_fetch_sub(&in_use_now, w);
        ws_release(w);
    }
    pthread_mutex_lock(&stat_mu);
    weight_total += mine;
    pthread_mutex_unlock(&stat_mu);
    return NULL;
}

int main(void) {
    /* Head-of-line demo: capacity 4, main holds 3. A wants 3, B wants 1 (which would fit right now). */
    ws_setup(4);
    ws_acquire(3);
    Req a = {1, 3}, b = {2, 1};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, demo_req, &a);
    wait_tickets(2);
    pthread_create(&tb, NULL, demo_req, &b);
    wait_tickets(3);
    for (int i = 0; i < 20000; i++)
        sched_yield();
    printf("B (1 permit) granted while A (3 permits) is ahead: %s\n", atomic_load(&grant_pos) > 0 ? "yes" : "no");
    check(atomic_load(&grant_pos) == 0, "no barging past the head");
    ws_release(3);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    printf("grant order: A then B = %s\n", grant_seq[0] == 1 && grant_seq[1] == 2 ? "yes" : "no");
    check(grant_seq[0] == 1 && grant_seq[1] == 2, "FIFO order");
    printf("permits in use after A and B: %d of %d\n", ws.capacity - ws.avail, ws.capacity);
    check(ws.avail == 0, "A and B hold all four");
    ws_release(4);
    check(ws.avail == 4, "restored");

    /* Stress: random weights against capacity 6. */
    ws.next_ticket = ws.head_ticket = 0;
    ws.first_grant = 1;
    ws.max_in_use = 0;
    ws.grant_order_bad = 0;
    ws_setup(CAP);
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, stress, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    long expect = 0;
    for (int id = 0; id < T; id++) {
        unsigned s = 5u + (unsigned)id * 977u;
        for (int i = 0; i < PER; i++) {
            s = s * 1664525u + 1013904223u;
            expect += 1 + (long)((s >> 20) % 4);
        }
    }
    printf("stress: %d grants, weight granted %ld (expected %ld)\n", T * PER, weight_total, expect);
    printf("over capacity observations %d, max in use within capacity: %s, grant order breaks %d\n",
           atomic_load(&over_cap), ws.max_in_use <= CAP ? "yes" : "no", ws.grant_order_bad);
    check(weight_total == expect && atomic_load(&over_cap) == 0, "stress totals");
    check(ws.max_in_use <= CAP && ws.grant_order_bad == 0, "bounds");
    check(ws.avail == CAP && ws.head_ticket == ws.next_ticket && ws.head_ticket == (unsigned)(T * PER), "idle");
    return 0;
}
