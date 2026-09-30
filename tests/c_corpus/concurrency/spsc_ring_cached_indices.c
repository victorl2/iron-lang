/*
 * title: SPSC ring buffer with cached indices and burst transfers
 * topic: concurrency
 * covers: single-producer single-consumer ring, cached remote index, alignas separation, burst enqueue/dequeue, order-sensitive checksum
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Each side owns one index (written only by that side) and keeps a private cached copy of the
 * other side's index. It reloads the remote index (acquire) only when the cache says the ring
 * looks full/empty, so the common path touches no shared cache line.
 *
 * Correctness argument: the producer writes slots then store-release(tail); the consumer's
 * load-acquire(tail) makes those slots visible. Symmetrically the consumer's store-release(head)
 * tells the producer that slots may be overwritten. A stale cache only underestimates free
 * space / available items, so it is safe.
 */
enum { CAP = 64, MASK = CAP - 1, TOTAL = 120000 };

typedef struct {
    alignas(64) atomic_size_t tail; /* written by producer */
    alignas(64) atomic_size_t head; /* written by consumer */
    alignas(64) size_t head_cache;  /* producer private */
    alignas(64) size_t tail_cache;  /* consumer private */
    alignas(64) uint32_t slot[CAP];
} Ring;

static Ring ring;
static uint64_t consumer_hash;
static unsigned long consumer_count;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static size_t produce(const uint32_t *src, size_t n) {
    size_t t = atomic_load_explicit(&ring.tail, memory_order_relaxed);
    size_t free_slots = CAP - (t - ring.head_cache);
    if (free_slots < n) {
        ring.head_cache = atomic_load_explicit(&ring.head, memory_order_acquire);
        free_slots = CAP - (t - ring.head_cache);
    }
    if (n > free_slots)
        n = free_slots;
    for (size_t i = 0; i < n; i++)
        ring.slot[(t + i) & MASK] = src[i];
    if (n)
        atomic_store_explicit(&ring.tail, t + n, memory_order_release);
    return n;
}

static size_t consume(uint32_t *dst, size_t max) {
    size_t h = atomic_load_explicit(&ring.head, memory_order_relaxed);
    size_t avail = ring.tail_cache - h;
    if (avail < max) {
        ring.tail_cache = atomic_load_explicit(&ring.tail, memory_order_acquire);
        avail = ring.tail_cache - h;
    }
    size_t n = avail < max ? avail : max;
    for (size_t i = 0; i < n; i++)
        dst[i] = ring.slot[(h + i) & MASK];
    if (n)
        atomic_store_explicit(&ring.head, h + n, memory_order_release);
    return n;
}

static uint32_t value_at(unsigned long i) {
    return (uint32_t)(i * 2654435761u) ^ (uint32_t)(i >> 3);
}

static void *producer(void *arg) {
    (void)arg;
    unsigned long sent = 0;
    uint32_t buf[8];
    unsigned burst_state = 1;
    while (sent < TOTAL) {
        burst_state = burst_state * 5 + 3;
        size_t want = 1 + (burst_state >> 4) % 7;
        if (want > TOTAL - sent)
            want = (size_t)(TOTAL - sent);
        for (size_t i = 0; i < want; i++)
            buf[i] = value_at(sent + i);
        size_t done = 0;
        while (done < want) {
            size_t k = produce(buf + done, want - done);
            done += k;
            if (k == 0)
                sched_yield();
        }
        sent += want;
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    uint64_t h = 1469598103934665603ull;
    unsigned long got = 0;
    uint32_t buf[5];
    while (got < TOTAL) {
        size_t k = consume(buf, 5);
        if (k == 0) {
            sched_yield();
            continue;
        }
        for (size_t i = 0; i < k; i++) {
            check(buf[i] == value_at(got + i), "value out of order");
            h = (h ^ buf[i]) * 1099511628211ull;
        }
        got += k;
    }
    consumer_hash = h;
    consumer_count = got;
    return NULL;
}

int main(void) {
    pthread_t p, c;
    check(pthread_create(&c, NULL, consumer, NULL) == 0, "create");
    check(pthread_create(&p, NULL, producer, NULL) == 0, "create");
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    uint64_t h = 1469598103934665603ull;
    for (unsigned long i = 0; i < TOTAL; i++)
        h = (h ^ value_at(i)) * 1099511628211ull;
    check(consumer_hash == h, "order-sensitive hash");
    check(atomic_load(&ring.head) == TOTAL && atomic_load(&ring.tail) == TOTAL, "indices");
    printf("transferred=%lu\n", consumer_count);
    printf("fnv1a of stream=%llu\n", (unsigned long long)consumer_hash);
    printf("ring empty at end: %s\n", atomic_load(&ring.head) == atomic_load(&ring.tail) ? "yes" : "no");
    return 0;
}
