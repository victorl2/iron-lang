/*
 * title: Count subarrays with a given sum using a prefix hash map
 * topic: algorithms
 * covers: prefix sums, open-addressing hash table, count of earlier prefixes, negative values, modular subarray divisibility
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 20260929ull;
static unsigned rnd(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    long long key;
    int count;
    int used;
} Slot;

typedef struct {
    Slot *slots;
    unsigned cap; /* power of two */
} Map;

static void map_init(Map *m, unsigned cap) {
    m->slots = calloc(cap, sizeof(Slot));
    if (!m->slots)
        fail("alloc");
    m->cap = cap;
}

static unsigned hash64(long long k) {
    unsigned long long x = (unsigned long long)k;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return (unsigned)x;
}

static Slot *map_slot(Map *m, long long key) {
    unsigned i = hash64(key) & (m->cap - 1);
    while (m->slots[i].used && m->slots[i].key != key)
        i = (i + 1) & (m->cap - 1);
    return &m->slots[i];
}

static int map_get(Map *m, long long key) {
    Slot *s = map_slot(m, key);
    return s->used ? s->count : 0;
}
static void map_inc(Map *m, long long key) {
    Slot *s = map_slot(m, key);
    if (!s->used) {
        s->used = 1;
        s->key = key;
    }
    s->count++;
}

static long count_sum_k(const int *a, int n, long long k) {
    Map m;
    map_init(&m, 4096);
    long long p = 0;
    long total = 0;
    map_inc(&m, 0);
    for (int i = 0; i < n; i++) {
        p += a[i];
        total += map_get(&m, p - k);
        map_inc(&m, p);
    }
    free(m.slots);
    return total;
}

/* subarrays whose sum is divisible by d, counting residues */
static long count_divisible(const int *a, int n, int d) {
    long *cnt = calloc((size_t)d, sizeof(long));
    if (!cnt)
        fail("alloc");
    long long p = 0;
    long total = 0;
    cnt[0] = 1;
    for (int i = 0; i < n; i++) {
        p += a[i];
        int r = (int)(((p % d) + d) % d);
        total += cnt[r];
        cnt[r]++;
    }
    free(cnt);
    return total;
}

int main(void) {
    int a[1500];
    for (int trial = 0; trial < 30; trial++) {
        int n = 1 + (int)(rnd() % 1200);
        int spread = 2 + (int)(rnd() % 9);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % (unsigned)(2 * spread + 1)) - spread;
        long long k = (long long)(rnd() % 11) - 5;
        long want = 0;
        for (int i = 0; i < n; i++) {
            long long s = 0;
            for (int j = i; j < n; j++) {
                s += a[j];
                if (s == k)
                    want++;
            }
        }
        if (count_sum_k(a, n, k) != want)
            fail("count_sum_k");
        int d = 2 + (int)(rnd() % 9);
        long wd = 0;
        for (int i = 0; i < n; i++) {
            long long s = 0;
            for (int j = i; j < n; j++) {
                s += a[j];
                if (s % d == 0)
                    wd++;
            }
        }
        if (count_divisible(a, n, d) != wd)
            fail("count_divisible");
        if (trial < 5)
            printf("trial %d: n=%d k=%lld matches=%ld, divisible by %d: %ld\n", trial, n, k, want,
                   d, wd);
    }
    int ones[] = {1, 1, 1};
    printf("[1,1,1] k=2 -> %ld\n", count_sum_k(ones, 3, 2));
    int zeros[] = {0, 0, 0, 0};
    printf("[0,0,0,0] k=0 -> %ld\n", count_sum_k(zeros, 4, 0));
    int mixed[] = {4, 5, 0, -2, -3, 1};
    printf("[4,5,0,-2,-3,1] divisible by 5 -> %ld\n", count_divisible(mixed, 6, 5));
    return 0;
}
