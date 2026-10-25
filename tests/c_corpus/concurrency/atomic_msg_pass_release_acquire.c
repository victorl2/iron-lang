/*
 * title: Message passing with release/acquire flags and a token-passing chain
 * topic: concurrency
 * covers: release store, acquire load, publication of plain payloads, happens-before chain across threads
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Correctness argument:
 * Part 1: the producer writes plain payload words, then store-release(ready[i], 1). A consumer
 * that load-acquires ready[i] == 1 synchronizes-with that store, so all payload writes
 * happen-before the consumer's reads: it can never see a half-written message.
 * Part 2: a plain counter is handed from thread to thread through a turn variable. Each hand-off
 * is a release store paired with an acquire load, which chains happens-before across all threads,
 * so the plain increments never race and none are lost.
 */
enum { MSGS = 200, WORDS = 8, NC = 3, RING = 4, ROUNDS = 500 };

typedef struct {
    unsigned w[WORDS];
} Msg;

static Msg box[MSGS];
static atomic_int ready[MSGS];
static unsigned long consumer_sum[NC];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned word_of(int i, int j) {
    unsigned x = (unsigned)(i * 31 + j) * 2654435761u;
    return x ^ (x >> 15);
}

static void *producer(void *arg) {
    (void)arg;
    for (int i = 0; i < MSGS; i++) {
        for (int j = 0; j < WORDS; j++)
            box[i].w[j] = word_of(i, j);
        atomic_store_explicit(&ready[i], 1, memory_order_release);
        if (i % 16 == 0)
            sched_yield();
    }
    return NULL;
}

static void *consumer(void *arg) {
    int id = (int)(size_t)arg;
    unsigned long sum = 0;
    for (int i = 0; i < MSGS; i++) {
        while (atomic_load_explicit(&ready[i], memory_order_acquire) == 0)
            sched_yield();
        for (int j = 0; j < WORDS; j++) {
            unsigned v = box[i].w[j];
            if (v != word_of(i, j)) {
                fprintf(stderr, "torn message %d word %d\n", i, j);
                exit(1);
            }
            sum += v;
        }
    }
    consumer_sum[id] = sum;
    return NULL;
}

static atomic_int turn;
static long shared_counter; /* plain, protected only by the turn hand-off */

static void *ring_worker(void *arg) {
    int id = (int)(size_t)arg;
    for (int r = 0; r < ROUNDS; r++) {
        while (atomic_load_explicit(&turn, memory_order_acquire) != id)
            sched_yield();
        shared_counter += id + 1;
        atomic_store_explicit(&turn, (id + 1) % RING, memory_order_release);
    }
    return NULL;
}

int main(void) {
    pthread_t p, c[NC];
    for (int i = 0; i < NC; i++)
        check(pthread_create(&c[i], NULL, consumer, (void *)(size_t)i) == 0, "create");
    check(pthread_create(&p, NULL, producer, NULL) == 0, "create");
    pthread_join(p, NULL);
    for (int i = 0; i < NC; i++)
        pthread_join(c[i], NULL);
    unsigned long expect = 0;
    for (int i = 0; i < MSGS; i++)
        for (int j = 0; j < WORDS; j++)
            expect += word_of(i, j);
    for (int i = 0; i < NC; i++) {
        check(consumer_sum[i] == expect, "consumer checksum");
        printf("consumer %d checksum %lu\n", i, consumer_sum[i]);
    }
    pthread_t r[RING];
    for (int i = 0; i < RING; i++)
        check(pthread_create(&r[i], NULL, ring_worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < RING; i++)
        pthread_join(r[i], NULL);
    long want = 0;
    for (int i = 0; i < RING; i++)
        want += (long)(i + 1) * ROUNDS;
    check(shared_counter == want, "token chain counter");
    printf("token chain counter %ld after %d hand-offs\n", shared_counter, RING * ROUNDS);
    return 0;
}
