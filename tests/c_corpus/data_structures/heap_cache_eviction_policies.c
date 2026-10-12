/*
 * title: Cache eviction policies on an indexed heap
 * topic: data_structures
 * covers: indexed heap, LFU, LRU, FIFO, pluggable priority, tie-breaking by tick, linear-scan oracle, hit ratio
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xCAC4Eull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef enum { LFU, LRU, FIFO } Policy;

enum { KEYS = 400, CAP = 40 };

/* priority is a pair (a, b) compared lexicographically; the smallest pair is evicted first:
 * LFU: (frequency, last tick)   LRU: (last tick, 0)   FIFO: (insertion tick, 0) */
typedef struct {
    long a, b;
} Pri;

static int pri_lt(Pri x, Pri y) { return x.a < y.a || (x.a == y.a && x.b < y.b); }

typedef struct {
    Policy pol;
    int heap[CAP], n;
    int pos[KEYS]; /* -1 when not cached */
    Pri pri[KEYS];
    long freq[KEYS], ins[KEYS];
    long tick, hits, misses, evictions;
} Cache;

static void place(Cache *c, int i, int key) {
    c->heap[i] = key;
    c->pos[key] = i;
}

static void up(Cache *c, int i) {
    int key = c->heap[i];
    while (i > 0 && pri_lt(c->pri[key], c->pri[c->heap[(i - 1) / 2]])) {
        place(c, i, c->heap[(i - 1) / 2]);
        i = (i - 1) / 2;
    }
    place(c, i, key);
}

static void down(Cache *c, int i) {
    int key = c->heap[i];
    for (;;) {
        int ch = 2 * i + 1;
        if (ch >= c->n)
            break;
        if (ch + 1 < c->n && pri_lt(c->pri[c->heap[ch + 1]], c->pri[c->heap[ch]]))
            ch++;
        if (!pri_lt(c->pri[c->heap[ch]], c->pri[key]))
            break;
        place(c, i, c->heap[ch]);
        i = ch;
    }
    place(c, i, key);
}

static Pri make_pri(const Cache *c, int key) {
    switch (c->pol) {
    case LFU:
        return (Pri){c->freq[key], c->tick};
    case LRU:
        return (Pri){c->tick, 0};
    default:
        return (Pri){c->ins[key], 0};
    }
}

static void cache_init(Cache *c, Policy p) {
    memset(c, 0, sizeof *c);
    c->pol = p;
    for (int i = 0; i < KEYS; i++)
        c->pos[i] = -1;
}

/* returns the evicted key or -1; sets *hit */
static int cache_access(Cache *c, int key, int *hit) {
    c->tick++;
    int evicted = -1;
    if (c->pos[key] >= 0) {
        *hit = 1;
        c->hits++;
        c->freq[key]++;
        c->pri[key] = make_pri(c, key);
        down(c, c->pos[key]);
        up(c, c->pos[key]);
        return -1;
    }
    *hit = 0;
    c->misses++;
    if (c->n == CAP) {
        evicted = c->heap[0];
        c->pos[evicted] = -1;
        c->n--;
        if (c->n > 0) {
            place(c, 0, c->heap[c->n]);
            down(c, 0);
        }
        c->evictions++;
    }
    c->freq[key] = 1;
    c->ins[key] = c->tick;
    c->pri[key] = make_pri(c, key);
    place(c, c->n++, key);
    up(c, c->n - 1);
    return evicted;
}

static void invariant(const Cache *c) {
    int present = 0;
    for (int k = 0; k < KEYS; k++)
        if (c->pos[k] >= 0) {
            present++;
            check(c->heap[c->pos[k]] == k, "position map");
        }
    check(present == c->n, "cached count");
    for (int i = 1; i < c->n; i++)
        check(!pri_lt(c->pri[c->heap[i]], c->pri[c->heap[(i - 1) / 2]]), "heap order");
}

/* oracle: the same policy with a plain array and a linear scan for the victim */
typedef struct {
    Policy pol;
    int keys[CAP], n;
    long freq[KEYS], ins[KEYS], last[KEYS], tick;
} Naive;

static int naive_access(Naive *o, int key, int *hit) {
    o->tick++;
    for (int i = 0; i < o->n; i++)
        if (o->keys[i] == key) {
            *hit = 1;
            o->freq[key]++;
            o->last[key] = o->tick;
            return -1;
        }
    *hit = 0;
    int evicted = -1;
    if (o->n == CAP) {
        int bi = 0;
        for (int i = 1; i < o->n; i++) {
            int a = o->keys[i], b = o->keys[bi];
            Pri pa, pb;
            if (o->pol == LFU) {
                pa = (Pri){o->freq[a], o->last[a]};
                pb = (Pri){o->freq[b], o->last[b]};
            } else if (o->pol == LRU) {
                pa = (Pri){o->last[a], 0};
                pb = (Pri){o->last[b], 0};
            } else {
                pa = (Pri){o->ins[a], 0};
                pb = (Pri){o->ins[b], 0};
            }
            if (pri_lt(pa, pb))
                bi = i;
        }
        evicted = o->keys[bi];
        o->keys[bi] = o->keys[--o->n];
    }
    o->keys[o->n++] = key;
    o->freq[key] = 1;
    o->ins[key] = o->tick;
    o->last[key] = o->tick;
    return evicted;
}

static int next_key(int workload, long t) {
    unsigned r = rng();
    if (workload == 0) { /* skewed: product of two uniforms concentrates on small keys */
        unsigned a = r % KEYS, b = rng() % KEYS;
        return (int)(a * b / KEYS);
    }
    if (workload == 1) { /* hot set plus a long sequential scan that pollutes recency */
        if ((t / 2000) % 2 == 1)
            return (int)(t % KEYS);
        return (int)(r % 25);
    }
    return (int)(r % KEYS); /* uniform */
}

int main(void) {
    const char *wname[3] = {"skewed", "hot set + scans", "uniform"};
    const char *pname[3] = {"LFU", "LRU", "FIFO"};
    printf("%-16s %-6s %-7s %-7s %-10s\n", "workload", "policy", "hits", "misses", "hit%");
    for (int w = 0; w < 3; w++) {
        for (int p = 0; p < 3; p++) {
            Cache c;
            Naive o;
            cache_init(&c, (Policy)p);
            memset(&o, 0, sizeof o);
            o.pol = (Policy)p;
            rs = 0xCAC4Eull + (uint64_t)w * 7919u; /* same stream for every policy */
            unsigned long long h = 1469598103934665603ull;
            for (long t = 0; t < 12000; t++) {
                int key = next_key(w, t), hit1, hit2;
                int e1 = cache_access(&c, key, &hit1);
                int e2 = naive_access(&o, key, &hit2);
                check(hit1 == hit2 && e1 == e2, "heap cache matches the linear-scan oracle");
                h = (h ^ (unsigned)(e1 + 2)) * 1099511628211ull;
                if (t % 251 == 0)
                    invariant(&c);
            }
            long total = c.hits + c.misses;
            printf("%-16s %-6s %-7ld %-7ld %ld.%02ld%%  eviction_hash=%llu\n", wname[w], pname[p], c.hits, c.misses, c.hits * 100 / total, c.hits * 10000 / total % 100, h % 100000);
        }
    }
    return 0;
}
