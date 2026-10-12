/*
 * title: Double-buffered producer and consumer with buffer ownership checks
 * topic: concurrency
 * covers: two buffers, EMPTY/FULL handoff states, overlap detection, per-round checksums, alternating ownership
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { BUFSZ = 64, ROUNDS = 40 };
typedef enum { EMPTY, FILLING, FULL, DRAINING } BState;

static unsigned buf[2][BUFSZ];
static BState state[2];
static int round_of[2];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned checksum[ROUNDS];
static int overlap_violations;
static int used_by_producer[2], used_by_consumer[2];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned gen(int round, int i) {
    unsigned h = (unsigned)round * 0x9e3779b1u ^ (unsigned)i * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

static void *producer(void *arg) {
    for (int r = 0; r < ROUNDS; r++) {
        int b = r % 2;
        pthread_mutex_lock(&mu);
        while (state[b] != EMPTY)
            pthread_cond_wait(&cv, &mu);
        state[b] = FILLING;
        /* the other buffer must not also be in FILLING */
        if (state[1 - b] == FILLING)
            overlap_violations++;
        used_by_producer[b]++;
        pthread_mutex_unlock(&mu);
        for (int i = 0; i < BUFSZ; i++)
            buf[b][i] = gen(r, i);
        pthread_mutex_lock(&mu);
        round_of[b] = r;
        state[b] = FULL;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void *consumer(void *arg) {
    for (int r = 0; r < ROUNDS; r++) {
        int b = r % 2;
        pthread_mutex_lock(&mu);
        while (state[b] != FULL)
            pthread_cond_wait(&cv, &mu);
        state[b] = DRAINING;
        if (state[1 - b] == DRAINING)
            overlap_violations++;
        if (round_of[b] != r)
            overlap_violations++;
        used_by_consumer[b]++;
        pthread_mutex_unlock(&mu);
        unsigned h = 17;
        for (int i = 0; i < BUFSZ; i++)
            h = h * 31u + buf[b][i];
        checksum[r] = h;
        pthread_mutex_lock(&mu);
        state[b] = EMPTY;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

int main(void) {
    pthread_t p, c;
    check(pthread_create(&c, NULL, consumer, NULL) == 0, "consumer");
    check(pthread_create(&p, NULL, producer, NULL) == 0, "producer");
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    check(overlap_violations == 0, "buffers handed over cleanly");
    unsigned all = 0;
    for (int r = 0; r < ROUNDS; r++) {
        unsigned h = 17;
        for (int i = 0; i < BUFSZ; i++)
            h = h * 31u + gen(r, i);
        check(h == checksum[r], "round checksum");
        all = all * 1000003u + h;
        if (r < 6 || r == ROUNDS - 1)
            printf("round %2d buffer %d checksum %08x\n", r, r % 2, h);
    }
    check(used_by_producer[0] == ROUNDS / 2 && used_by_producer[1] == ROUNDS / 2, "producer alternates");
    check(used_by_consumer[0] == ROUNDS / 2 && used_by_consumer[1] == ROUNDS / 2, "consumer alternates");
    printf("rounds %d, each buffer used %d times by each side\n", ROUNDS, ROUNDS / 2);
    printf("combined checksum %08x\n", all);
    return 0;
}
