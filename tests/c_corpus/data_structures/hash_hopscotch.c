/*
 * title: Hopscotch hashing with neighborhood bitmaps
 * topic: data_structures
 * covers: hopscotch hashing, neighborhood bitmap, free-slot displacement, bit scanning, resize on failure
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
enum { H = 32, ADD_RANGE = 256 };
typedef struct {
    uint32_t key;
    int val;
    uint32_t hop; /* bit i set: slot (home+i) holds a key whose home is this slot */
    uint8_t used;
} Slot;
typedef struct {
    Slot *s;
    size_t cap, n; /* cap is a power of two; probing wraps */
    long moves, grows, scanned;
} Tab;

static size_t home(const Tab *t, uint32_t k) { return mix32(k) & (t->cap - 1); }

static void tab_init(Tab *t, size_t cap) {
    t->s = calloc(cap, sizeof(Slot));
    t->cap = cap;
    t->n = 0;
}

static long tab_find(const Tab *t, uint32_t k) {
    size_t h = home(t, k);
    uint32_t hop = t->s[h].hop;
    while (hop) {
        int b = __builtin_ctz(hop);
        size_t i = (h + (size_t)b) & (t->cap - 1);
        if (t->s[i].key == k)
            return (long)i;
        hop &= hop - 1;
    }
    return -1;
}

static int tab_try_insert(Tab *t, uint32_t k, int v) {
    size_t mask = t->cap - 1, h = home(t, k), free_i = h;
    size_t dist = 0;
    while (t->s[free_i].used) {
        free_i = (free_i + 1) & mask;
        dist++;
        t->scanned++;
        if (dist >= ADD_RANGE)
            return 0;
    }
    while (dist >= H) {
        /* find an item in the H-1 slots before free_i that can hop into free_i */
        int moved = 0;
        for (size_t back = H - 1; back >= 1 && !moved; back--) {
            size_t cand = (free_i + t->cap - back) & mask; /* candidate home */
            uint32_t hop = t->s[cand].hop;
            while (hop) {
                int b = __builtin_ctz(hop);
                if ((size_t)b >= back)
                    break;
                size_t from = (cand + (size_t)b) & mask;
                t->s[free_i].key = t->s[from].key;
                t->s[free_i].val = t->s[from].val;
                t->s[free_i].used = 1;
                t->s[cand].hop &= ~(1u << b);
                t->s[cand].hop |= 1u << back;
                t->s[from].used = 0;
                free_i = from;
                dist -= back - (size_t)b;
                t->moves++;
                moved = 1;
                break;
            }
        }
        if (!moved)
            return 0;
    }
    t->s[free_i].used = 1;
    t->s[free_i].key = k;
    t->s[free_i].val = v;
    t->s[h].hop |= 1u << dist;
    t->n++;
    return 1;
}

static void tab_put(Tab *t, uint32_t k, int v) {
    long f = tab_find(t, k);
    if (f >= 0) {
        t->s[f].val = v;
        return;
    }
    while (!tab_try_insert(t, k, v)) {
        Tab n;
        tab_init(&n, t->cap * 2);
        n.moves = t->moves;
        n.grows = t->grows + 1;
        n.scanned = t->scanned;
        for (size_t i = 0; i < t->cap; i++)
            if (t->s[i].used)
                check(tab_try_insert(&n, t->s[i].key, t->s[i].val), "regrow insert");
        free(t->s);
        *t = n;
    }
}

static int tab_del(Tab *t, uint32_t k) {
    long f = tab_find(t, k);
    if (f < 0)
        return 0;
    size_t h = home(t, k), d = ((size_t)f - h) & (t->cap - 1);
    t->s[h].hop &= ~(1u << d);
    t->s[f].used = 0;
    t->n--;
    return 1;
}

int main(void) {
    Tab t;
    memset(&t, 0, sizeof t);
    tab_init(&t, 64);
    for (int step = 0; step < 30000; step++) {
        uint32_t k = (uint32_t)(rnd() % 3000);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 5) {
            int v = (int)(rnd() & 0xffff);
            /* keep table near 90 percent load before allowing growth to show displacement work */
            tab_put(&t, k, v);
            ref_put(k, v);
        } else if (op < 8) {
            check(tab_del(&t, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            long f = tab_find(&t, k);
            check((f >= 0) == (ri >= 0), "find");
            if (f >= 0)
                check(t.s[f].val == ref_v[ri], "val");
        }
        check(t.n == (size_t)ref_n, "size");
    }
    /* invariants: every hop bit points at a used slot whose home is the bitmap owner */
    size_t bits = 0;
    int maxd = 0;
    for (size_t i = 0; i < t.cap; i++) {
        uint32_t hop = t.s[i].hop;
        while (hop) {
            int b = __builtin_ctz(hop);
            size_t j = (i + (size_t)b) & (t.cap - 1);
            check(t.s[j].used && home(&t, t.s[j].key) == i, "hop bit consistent");
            if (b > maxd)
                maxd = b;
            bits++;
            hop &= hop - 1;
        }
    }
    check(bits == t.n, "bit count equals size");
    printf("n=%zu cap=%zu load=%.4f grows=%ld\n", t.n, t.cap, (double)t.n / (double)t.cap, t.grows);
    printf("hop moves=%ld free-slot scan steps=%ld max hop distance=%d (H=%d)\n", t.moves, t.scanned, maxd, H);
    free(t.s);
    return 0;
}
