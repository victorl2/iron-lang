/*
 * title: Exchanger pairing threads to swap values
 * topic: concurrency
 * covers: exchanger, rendezvous of two threads, slot states, pairing involution check, value conservation
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int state; /* 0 empty, 1 first party parked, 2 second party arrived, reply ready */
    long first_val, second_val;
    unsigned generation;
    long exchanges;
} Exchanger;

static Exchanger ex = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0, 0, 0};

/* Blocks until another thread arrives, then returns that thread's value. */
static long exchange(long mine) {
    long theirs;
    pthread_mutex_lock(&ex.mu);
    /* wait until the slot is free for a new pair (previous pair may still be departing) */
    while (ex.state == 2)
        pthread_cond_wait(&ex.cv, &ex.mu);
    if (ex.state == 0) { /* first arrival parks its value */
        ex.state = 1;
        ex.first_val = mine;
        unsigned gen = ex.generation;
        while (ex.state != 2 || ex.generation != gen)
            pthread_cond_wait(&ex.cv, &ex.mu);
        theirs = ex.second_val;
        ex.state = 0; /* first party leaves last: the slot reopens */
        ex.generation++;
        ex.exchanges++;
        pthread_cond_broadcast(&ex.cv);
    } else { /* second arrival takes the parked value and leaves its own */
        theirs = ex.first_val;
        ex.second_val = mine;
        ex.state = 2;
        pthread_cond_broadcast(&ex.cv);
    }
    pthread_mutex_unlock(&ex.mu);
    return theirs;
}

/* Round barrier: keeps every thread in the same round so that each round pairs up all T threads. */
static pthread_mutex_t bmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv = PTHREAD_COND_INITIALIZER;
static int barrived;
static unsigned bgen;
static void round_barrier(int parties) {
    pthread_mutex_lock(&bmu);
    unsigned g = bgen;
    if (++barrived == parties) {
        barrived = 0;
        bgen++;
        pthread_cond_broadcast(&bcv);
    } else {
        while (g == bgen)
            pthread_cond_wait(&bcv, &bmu);
    }
    pthread_mutex_unlock(&bmu);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { T = 6, ROUNDS = 100 };

/* A value encodes (thread, round): key = thread * 1000 + round. */
static long got[T][ROUNDS];

static void *worker(void *p) {
    int id = (int)(intptr_t)p;
    for (int r = 0; r < ROUNDS; r++) {
        got[id][r] = exchange((long)id * 1000 + r);
        if ((id + r) % 7 == 0)
            sched_yield();
        round_barrier(T);
    }
    return NULL;
}

static long duo_a, duo_b;
static void *duo(void *p) {
    long *slot = p;
    long mine = slot == &duo_a ? 111 : 222;
    *slot = exchange(mine);
    return NULL;
}

int main(void) {
    pthread_t x, y;
    pthread_create(&x, NULL, duo, &duo_a);
    pthread_create(&y, NULL, duo, &duo_b);
    pthread_join(x, NULL);
    pthread_join(y, NULL);
    printf("pair swap: a received %ld, b received %ld\n", duo_a, duo_b);
    check(duo_a == 222 && duo_b == 111, "pair swap");

    pthread_t th[T];
    for (int i = 0; i < T; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    /* Each thread sent (id, r) and received (j, r2). The partner must have received (id, r) at its round r2. */
    int asymmetric = 0, self_swaps = 0, bad_range = 0;
    long sent_sum = 0, recv_sum = 0;
    int partner_hist[T] = {0};
    for (int i = 0; i < T; i++) {
        for (int r = 0; r < ROUNDS; r++) {
            long v = got[i][r];
            int j = (int)(v / 1000), r2 = (int)(v % 1000);
            if (j < 0 || j >= T || r2 < 0 || r2 >= ROUNDS) {
                bad_range++;
                continue;
            }
            if (j == i)
                self_swaps++;
            if (r2 != r)
                bad_range++; /* partners must come from the same round */
            if (got[j][r2] != (long)i * 1000 + r)
                asymmetric++;
            partner_hist[j]++;
            sent_sum += (long)i * 1000 + r;
            recv_sum += v;
        }
    }
    int total_recv = 0;
    for (int i = 0; i < T; i++)
        total_recv += partner_hist[i];
    printf("threads=%d rounds=%d exchanges=%ld\n", T, ROUNDS, ex.exchanges - 1);
    printf("received %d values, out of range %d, self swaps %d, asymmetric pairs %d\n", total_recv, bad_range,
           self_swaps, asymmetric);
    printf("value sums equal: %s (%ld)\n", sent_sum == recv_sum ? "yes" : "no", sent_sum);
    check(bad_range == 0 && self_swaps == 0 && asymmetric == 0, "pairing involution");
    check(sent_sum == recv_sum && total_recv == T * ROUNDS, "conservation");
    check(ex.exchanges - 1 == T * ROUNDS / 2, "exchange count");
    check(ex.state == 0, "slot empty");
    return 0;
}
