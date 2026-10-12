/*
 * title: Robin Hood hashing with backward-shift deletion
 * topic: data_structures
 * covers: robin hood hashing, probe sequence length, displacement stealing, backward shift, variance comparison
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
typedef struct {
    uint32_t key;
    int val;
    int dist; /* -1 = empty */
} Slot;
typedef struct {
    Slot *s;
    size_t cap, n;
    int robin;
} Tab;

static void tab_init(Tab *t, size_t cap, int robin) {
    t->s = malloc(cap * sizeof(Slot));
    for (size_t i = 0; i < cap; i++)
        t->s[i].dist = -1;
    t->cap = cap;
    t->n = 0;
    t->robin = robin;
}

static long tab_find(const Tab *t, uint32_t k) {
    size_t mask = t->cap - 1, i = mix32(k) & mask;
    for (int d = 0;; d++) {
        if (t->s[i].dist < 0)
            return -1;
        if (t->robin && t->s[i].dist < d)
            return -1; /* early exit: would have stolen this slot */
        if (t->s[i].key == k)
            return (long)i;
        i = (i + 1) & mask;
    }
}

static void tab_put(Tab *t, uint32_t k, int v) {
    long f = tab_find(t, k);
    if (f >= 0) {
        t->s[f].val = v;
        return;
    }
    if ((t->n + 1) * 10 > t->cap * 9) {
        check(0, "load cap");
    }
    size_t mask = t->cap - 1, i = mix32(k) & mask;
    Slot cur = {k, v, 0};
    for (;;) {
        if (t->s[i].dist < 0) {
            t->s[i] = cur;
            break;
        }
        if (t->robin && t->s[i].dist < cur.dist) {
            Slot tmp = t->s[i];
            t->s[i] = cur;
            cur = tmp;
        }
        i = (i + 1) & mask;
        cur.dist++;
    }
    t->n++;
}

static int tab_del(Tab *t, uint32_t k) {
    long f = tab_find(t, k);
    if (f < 0)
        return 0;
    size_t mask = t->cap - 1, i = (size_t)f;
    if (t->robin) {
        for (;;) {
            size_t j = (i + 1) & mask;
            if (t->s[j].dist <= 0)
                break;
            t->s[i] = t->s[j];
            t->s[i].dist--;
            i = j;
        }
        t->s[i].dist = -1;
    } else {
        /* plain linear: use generic backshift by re-inserting the run */
        t->s[i].dist = -1;
        size_t j = (i + 1) & mask;
        while (t->s[j].dist >= 0) {
            Slot mv = t->s[j];
            t->s[j].dist = -1;
            t->n--;
            tab_put(t, mv.key, mv.val);
            j = (j + 1) & mask;
        }
    }
    t->n--;
    return 1;
}

static void stats(const Tab *t, const char *name) {
    long sum = 0, sq = 0;
    int mx = 0;
    for (size_t i = 0; i < t->cap; i++)
        if (t->s[i].dist >= 0) {
            long d = t->s[i].dist;
            sum += d;
            sq += d * d;
            if (d > mx)
                mx = (int)d;
        }
    double mean = (double)sum / (double)t->n;
    double var = (double)sq / (double)t->n - mean * mean;
    printf("%-11s n=%zu mean displacement=%.4f variance=%.4f max=%d\n", name, t->n, mean, var, mx);
}

int main(void) {
    Tab tr, tl;
    tab_init(&tr, 4096, 1);
    tab_init(&tl, 4096, 0);
    for (int step = 0; step < 30000; step++) {
        uint32_t k = (uint32_t)(rnd() % 4500);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 5 && (ri >= 0 || ref_n < 3200)) {
            int v = (int)(rnd() & 0xffff);
            tab_put(&tr, k, v);
            tab_put(&tl, k, v);
            ref_put(k, v);
        } else if (op < 8) {
            int a = tab_del(&tr, k), b = tab_del(&tl, k);
            check(a == (ri >= 0) && b == a, "del");
            ref_del(k);
        } else {
            long a = tab_find(&tr, k), b = tab_find(&tl, k);
            check((a >= 0) == (ri >= 0) && (b >= 0) == (ri >= 0), "find");
            if (ri >= 0)
                check(tr.s[a].val == ref_v[ri] && tl.s[b].val == ref_v[ri], "val");
        }
        check(tr.n == (size_t)ref_n && tl.n == (size_t)ref_n, "size");
    }
    /* robin hood invariant: displacement never drops by more than 1 along a run */
    for (size_t i = 0; i < tr.cap; i++) {
        const Slot *a = &tr.s[i], *b = &tr.s[(i + 1) & (tr.cap - 1)];
        if (b->dist > 0)
            check(a->dist >= 0 && b->dist <= a->dist + 1, "robin invariant");
    }
    stats(&tr, "robin hood");
    stats(&tl, "linear");
    printf("load=%.4f\n", (double)tr.n / (double)tr.cap);
    free(tr.s);
    free(tl.s);
    return 0;
}
