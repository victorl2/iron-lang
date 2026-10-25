/*
 * title: Insert-only lock-free hash set with open addressing
 * topic: concurrency
 * covers: CAS on empty slot, linear probing, duplicate detection, concurrent contains, order-independent checksum
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Slots hold a nonzero 64-bit key, 0 meaning empty. Inserting means probing linearly from
 * hash(key) and CASing 0 -> key into the first empty slot; if a slot already holds the key the
 * insert is a duplicate. Slots never change once set (insert-only), so a probe can stop at the
 * first 0 for "absent" and no ABA or tombstones exist.
 *
 * Correctness argument: two threads inserting the same key probe the same sequence; the first to
 * CAS wins, the loser reloads the slot, sees its own key and reports duplicate. So the number of
 * successful inserts over all threads equals the number of distinct keys.
 */
enum { CAP = 8192, MASK = CAP - 1, T = 5, KEYS_PER = 1500 };

static atomic_uint_least64_t table[CAP];
static atomic_long inserted[T];
static atomic_long dups[T];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

/* returns 1 if newly inserted, 0 if already present */
static int insert(uint64_t key) {
    size_t i = (size_t)mix(key) & MASK;
    for (size_t probes = 0; probes < CAP; probes++, i = (i + 1) & MASK) {
        uint64_t cur = atomic_load_explicit(&table[i], memory_order_acquire);
        if (cur == 0) {
            if (atomic_compare_exchange_strong_explicit(&table[i], &cur, key, memory_order_acq_rel,
                                                        memory_order_acquire))
                return 1;
        }
        if (cur == key)
            return 0;
    }
    fprintf(stderr, "table full\n");
    exit(1);
}

static int contains(uint64_t key) {
    size_t i = (size_t)mix(key) & MASK;
    for (size_t probes = 0; probes < CAP; probes++, i = (i + 1) & MASK) {
        uint64_t cur = atomic_load_explicit(&table[i], memory_order_acquire);
        if (cur == 0)
            return 0;
        if (cur == key)
            return 1;
    }
    return 0;
}

/* key stream: each thread draws from a range shared with its neighbors, so duplicates abound */
static uint64_t key_of(int t, int i) {
    uint64_t base = (uint64_t)t * 900; /* ranges of 1500 overlap by 600 with the next thread */
    return 1 + base + (mix((uint64_t)(t * 100003 + i)) % KEYS_PER);
}

static void *worker(void *p) {
    int t = (int)(size_t)p;
    long ins = 0, dup = 0;
    for (int i = 0; i < KEYS_PER; i++) {
        if (insert(key_of(t, i)))
            ins++;
        else
            dup++;
        (void)contains(key_of(t, i / 2)); /* concurrent reads while others insert */
    }
    inserted[t] = ins;
    dups[t] = dup;
    return NULL;
}

int main(void) {
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    static unsigned char ref[T * 900 + KEYS_PER + 2];
    long distinct = 0;
    uint64_t want_sum = 0;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < KEYS_PER; i++) {
            uint64_t k = key_of(t, i);
            if (!ref[k]) {
                ref[k] = 1;
                distinct++;
                want_sum += k;
            }
        }
    long ins = 0, dup = 0;
    for (int t = 0; t < T; t++) {
        ins += inserted[t];
        dup += dups[t];
    }
    long slots = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < CAP; i++) {
        uint64_t k = atomic_load(&table[i]);
        if (k) {
            slots++;
            sum += k;
        }
    }
    check(ins == distinct, "successful inserts equal distinct keys");
    check(slots == distinct, "occupied slots equal distinct keys");
    check(sum == want_sum, "key checksum");
    check(ins + dup == (long)T * KEYS_PER, "every attempt accounted");
    int present = 0, absent = 0;
    for (uint64_t k = 1; k < (uint64_t)sizeof ref; k++) {
        int c = contains(k);
        check(c == (ref[k] != 0), "contains matches reference");
        if (c) present++; else absent++;
    }
    printf("attempts=%d distinct=%ld key_sum=%llu\n", T * KEYS_PER, distinct, (unsigned long long)sum);
    printf("contains: present=%d absent=%d\n", present, absent);
    return 0;
}
