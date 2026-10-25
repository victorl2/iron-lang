/*
 * title: SPSC byte ring carrying length-prefixed messages with wrap markers
 * topic: concurrency
 * covers: variable-size records, 2-byte length header, wrap padding marker, free-running indices, release/acquire publication, wrap count
 * deps: libc, pthread
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The ring holds bytes. A record is [len lo][len hi][payload...]. Records never straddle the
 * end of the buffer: if a record does not fit in the contiguous space before the end, the
 * producer writes a marker header 0xFFFF (if 2 bytes are left) and skips to the start; the
 * consumer follows the same rule (a tail of fewer than 2 bytes is skipped implicitly).
 *
 * head and tail are free-running byte counters (never reduced), so full = tail - head == CAP and
 * empty = tail == head. The producer publishes with store-release(tail) after writing the bytes;
 * the consumer publishes free space with store-release(head) after reading them.
 *
 * Determinism: where the producer wraps depends only on the sequence of message sizes (not on
 * the consumer), so the number of wrap markers is a pure function of the input and is compared
 * against a sequential simulation.
 */
enum { CAP = 512, MASK = CAP - 1, MSGS = 30000, MAXLEN = 60, MARKER = 0xffff };

static unsigned char ring[CAP];
static atomic_size_t head, tail;
static long wraps_produced, wraps_consumed;
static unsigned long payload_bytes;
static uint64_t consumer_hash;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static unsigned len_of(int i) {
    unsigned x = (unsigned)i * 2246822519u;
    x ^= x >> 15;
    return 1u + x % MAXLEN;
}
static unsigned char byte_of(int i, unsigned j) {
    return (unsigned char)((unsigned)i * 31u + j * 7u + 3u);
}

static void wait_space(size_t t, size_t need) {
    while (CAP - (t - atomic_load_explicit(&head, memory_order_acquire)) < need)
        sched_yield();
}

static void *producer(void *arg) {
    (void)arg;
    size_t t = atomic_load_explicit(&tail, memory_order_relaxed);
    for (int i = 0; i < MSGS; i++) {
        unsigned len = len_of(i);
        size_t need = 2 + len;
        size_t pos = t & MASK;
        size_t room = CAP - pos;
        if (need > room) { /* pad to the end of the buffer */
            wait_space(t, room + need);
            if (room >= 2) {
                ring[pos] = 0xff;
                ring[pos + 1] = 0xff;
            }
            t += room;
            wraps_produced++;
            pos = 0;
        } else {
            wait_space(t, need);
        }
        ring[pos] = (unsigned char)(len & 0xff);
        ring[pos + 1] = (unsigned char)(len >> 8);
        for (unsigned j = 0; j < len; j++)
            ring[pos + 2 + j] = byte_of(i, j);
        t += need;
        atomic_store_explicit(&tail, t, memory_order_release);
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    size_t h = 0;
    uint64_t hash = 1469598103934665603ull;
    int got = 0;
    while (got < MSGS) {
        size_t tl;
        while ((tl = atomic_load_explicit(&tail, memory_order_acquire)) == h)
            sched_yield();
        size_t pos = h & MASK, room = CAP - pos;
        if (room < 2) { /* implicit skip: too small for a header */
            h += room;
            atomic_store_explicit(&head, h, memory_order_release);
            wraps_consumed++;
            continue;
        }
        unsigned len = ring[pos] | ((unsigned)ring[pos + 1] << 8);
        if (len == MARKER) {
            h += room;
            atomic_store_explicit(&head, h, memory_order_release);
            wraps_consumed++;
            continue;
        }
        check(len == len_of(got), "length header");
        for (unsigned j = 0; j < len; j++) {
            unsigned char b = ring[pos + 2 + j];
            check(b == byte_of(got, j), "payload byte");
            hash = (hash ^ b) * 1099511628211ull;
        }
        payload_bytes += len;
        h += 2 + len;
        atomic_store_explicit(&head, h, memory_order_release);
        got++;
    }
    consumer_hash = hash;
    return NULL;
}

int main(void) {
    pthread_t p, c;
    check(pthread_create(&c, NULL, consumer, NULL) == 0, "create");
    check(pthread_create(&p, NULL, producer, NULL) == 0, "create");
    pthread_join(p, NULL);
    pthread_join(c, NULL);
    /* sequential simulation of the producer's layout */
    size_t t = 0;
    long sim_wraps = 0;
    unsigned long sim_bytes = 0;
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < MSGS; i++) {
        unsigned len = len_of(i);
        size_t need = 2 + len, room = CAP - (t & MASK);
        if (need > room) {
            t += room;
            sim_wraps++;
        }
        t += need;
        sim_bytes += len;
        for (unsigned j = 0; j < len; j++)
            hash = (hash ^ byte_of(i, j)) * 1099511628211ull;
    }
    check(wraps_produced == sim_wraps, "wrap count matches simulation");
    check(wraps_consumed == sim_wraps, "consumer saw every wrap");
    check(payload_bytes == sim_bytes, "payload bytes");
    check(consumer_hash == hash, "stream hash");
    check(atomic_load(&head) == atomic_load(&tail) && atomic_load(&tail) == t, "indices");
    printf("messages=%d payload bytes=%lu\n", MSGS, payload_bytes);
    printf("wrap markers or skips=%ld, ring passes=%lu\n", sim_wraps, (unsigned long)(t / CAP));
    printf("stream fnv1a=%llu\n", (unsigned long long)consumer_hash);
    return 0;
}
