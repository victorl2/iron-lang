/*
 * title: Double hashing on a prime-sized table
 * topic: data_structures
 * covers: double hashing, prime table size, secondary hash step, string keys, probe sequence coverage
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
#define NKEYS 700
typedef struct {
    const char *key; /* NULL = empty */
    int val;
} Slot;
typedef struct {
    Slot *s;
    size_t cap, n;
    long probes, ops;
} Tab;

static uint32_t h1(const char *s) { /* FNV-1a */
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}
static uint32_t h2(const char *s) { /* djb2 variant */
    uint32_t h = 5381u;
    for (; *s; s++)
        h = h * 33u + (unsigned char)*s;
    return mix32(h);
}

static int is_prime(size_t n) {
    if (n < 2)
        return 0;
    for (size_t d = 2; d * d <= n; d++)
        if (n % d == 0)
            return 0;
    return 1;
}
static size_t next_prime(size_t n) {
    while (!is_prime(n))
        n++;
    return n;
}

static void tab_init(Tab *t, size_t cap) {
    t->cap = next_prime(cap);
    t->s = calloc(t->cap, sizeof(Slot));
    t->n = 0;
    t->probes = t->ops = 0;
}

static long tab_find(Tab *t, const char *k) {
    size_t i = h1(k) % t->cap, step = 1 + h2(k) % (t->cap - 1);
    t->ops++;
    for (size_t c = 0; c < t->cap; c++) {
        t->probes++;
        if (!t->s[i].key)
            return -1;
        if (strcmp(t->s[i].key, k) == 0)
            return (long)i;
        i = (i + step) % t->cap;
    }
    return -1;
}

static void tab_put(Tab *t, const char *k, int v) {
    long f = tab_find(t, k);
    if (f >= 0) {
        t->s[f].val = v;
        return;
    }
    if ((t->n + 1) * 10 > t->cap * 7) {
        Tab nt;
        tab_init(&nt, t->cap * 2);
        for (size_t i = 0; i < t->cap; i++)
            if (t->s[i].key)
                tab_put(&nt, t->s[i].key, t->s[i].val);
        nt.probes = t->probes;
        nt.ops = t->ops;
        free(t->s);
        *t = nt;
    }
    size_t i = h1(k) % t->cap, step = 1 + h2(k) % (t->cap - 1);
    while (t->s[i].key)
        i = (i + step) % t->cap;
    t->s[i].key = k;
    t->s[i].val = v;
    t->n++;
}

int main(void) {
    /* every probe sequence over a prime table is a full cycle */
    size_t p = 101;
    for (size_t step = 1; step < p; step++) {
        uint8_t seen[101] = {0};
        size_t i = 7;
        for (size_t c = 0; c < p; c++) {
            check(!seen[i], "cycle");
            seen[i] = 1;
            i = (i + step) % p;
        }
    }
    printf("all %zu step sizes give a full cycle mod %zu\n", p - 1, p);

    static char names[NKEYS][16];
    for (int i = 0; i < NKEYS; i++) {
        int len = 3 + (int)(rnd() % 6);
        for (int j = 0; j < len; j++)
            names[i][j] = (char)('a' + rnd() % 26);
        names[i][len] = 0;
    }
    Tab t;
    tab_init(&t, 11);
    int distinct = 0;
    for (int i = 0; i < NKEYS; i++) {
        int first = 1;
        for (int j = 0; j < i; j++)
            if (strcmp(names[i], names[j]) == 0)
                first = 0;
        tab_put(&t, names[i], i);
        distinct += first;
        /* later duplicates overwrite: value is the last index */
    }
    check(t.n == (size_t)distinct, "distinct count");
    for (int i = 0; i < NKEYS; i++) {
        int last = i;
        for (int j = i + 1; j < NKEYS; j++)
            if (strcmp(names[i], names[j]) == 0)
                last = j;
        long f = tab_find(&t, names[i]);
        check(f >= 0 && t.s[f].val == last, "lookup");
    }
    check(tab_find(&t, "zzzzzzzzzzzz") < 0, "absent");
    printf("distinct=%d cap=%zu (prime) load=%.4f\n", distinct, t.cap, (double)t.n / (double)t.cap);
    printf("avg probes=%.4f\n", (double)t.probes / (double)t.ops);
    printf("sample: %s=%d %s=%d\n", names[0], t.s[tab_find(&t, names[0])].val, names[NKEYS - 1],
           t.s[tab_find(&t, names[NKEYS - 1])].val);
    free(t.s);
    return 0;
}
