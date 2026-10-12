/*
 * title: Swiss-table style map with control bytes and SWAR group probing
 * topic: data_structures
 * covers: swiss table, control bytes, h1/h2 split, SWAR byte matching, empty vs deleted markers, group probing
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
enum { G = 8, CTRL_EMPTY = 0x80, CTRL_DEL = 0xFE };
#define LO 0x0101010101010101ull
#define HI 0x8080808080808080ull

typedef struct {
    uint8_t *ctrl;
    uint32_t *key;
    int *val;
    size_t ngroups, n, deleted;
    long group_probes, ops, rehashes;
} Tab;

static uint64_t load_group(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v; /* byte order does not matter: we only count/inspect bytes via ctrl[] */
}
/* bit 7 of each byte set where the byte equals b (may include false positives above a true match) */
static uint64_t match_byte(uint64_t grp, uint8_t b) {
    uint64_t x = grp ^ (LO * b);
    return (x - LO) & ~x & HI;
}

static void tab_init(Tab *t, size_t ngroups) {
    t->ngroups = ngroups;
    t->ctrl = malloc(ngroups * G);
    memset(t->ctrl, CTRL_EMPTY, ngroups * G);
    t->key = calloc(ngroups * G, sizeof(uint32_t));
    t->val = calloc(ngroups * G, sizeof(int));
    t->n = t->deleted = 0;
}
static void tab_free(Tab *t) {
    free(t->ctrl);
    free(t->key);
    free(t->val);
}

static long tab_find(Tab *t, uint32_t k) {
    uint64_t h = mix64(k);
    uint8_t h2 = (uint8_t)(h & 0x7f);
    size_t g = (size_t)(h >> 7) & (t->ngroups - 1);
    t->ops++;
    for (size_t step = 1;; step++) {
        t->group_probes++;
        uint64_t grp = load_group(&t->ctrl[g * G]);
        uint64_t m = match_byte(grp, h2);
        for (size_t i = 0; i < G; i++)
            if ((m >> (i * 8 + 7) & 1) && t->ctrl[g * G + i] == h2 && t->key[g * G + i] == k)
                return (long)(g * G + i);
        if (match_byte(grp, CTRL_EMPTY))
            return -1; /* group has an empty slot: key is absent */
        g = (g + step) & (t->ngroups - 1); /* triangular over groups */
    }
}

static void insert_new(Tab *t, uint32_t k, int v) {
    uint64_t h = mix64(k);
    size_t g = (size_t)(h >> 7) & (t->ngroups - 1);
    for (size_t step = 1;; step++) {
        for (size_t i = 0; i < G; i++) {
            uint8_t c = t->ctrl[g * G + i];
            if (c >= 0x80) { /* empty or deleted */
                if (c == CTRL_DEL)
                    t->deleted--;
                t->ctrl[g * G + i] = (uint8_t)(h & 0x7f);
                t->key[g * G + i] = k;
                t->val[g * G + i] = v;
                t->n++;
                return;
            }
        }
        g = (g + step) & (t->ngroups - 1);
    }
}

static void tab_put(Tab *t, uint32_t k, int v) {
    long f = tab_find(t, k);
    if (f >= 0) {
        t->val[f] = v;
        return;
    }
    size_t cap = t->ngroups * G;
    if ((t->n + t->deleted + 1) * 8 > cap * 7) { /* 7/8 max load, like real Swiss tables */
        Tab nt;
        tab_init(&nt, t->n * 2 > cap * 7 / 8 / 2 ? t->ngroups * 2 : t->ngroups);
        for (size_t i = 0; i < cap; i++)
            if (t->ctrl[i] < 0x80)
                insert_new(&nt, t->key[i], t->val[i]);
        nt.group_probes = t->group_probes;
        nt.ops = t->ops;
        nt.rehashes = t->rehashes + 1;
        tab_free(t);
        *t = nt;
    }
    insert_new(t, k, v);
}

static int tab_del(Tab *t, uint32_t k) {
    long f = tab_find(t, k);
    if (f < 0)
        return 0;
    size_t g = (size_t)f / G;
    /* if the group still has an empty slot no probe ever went past it, so EMPTY is safe */
    if (match_byte(load_group(&t->ctrl[g * G]), CTRL_EMPTY)) {
        t->ctrl[f] = CTRL_EMPTY;
    } else {
        t->ctrl[f] = CTRL_DEL;
        t->deleted++;
    }
    t->n--;
    return 1;
}

int main(void) {
    Tab t;
    memset(&t, 0, sizeof t);
    tab_init(&t, 2);
    long false_pos_checked = 0;
    for (int step = 0; step < 40000; step++) {
        uint32_t k = (uint32_t)(rnd() % 4000);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 4) {
            int v = (int)(rnd() & 0xffff);
            tab_put(&t, k, v);
            ref_put(k, v);
        } else if (op < 7) {
            check(tab_del(&t, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            long f = tab_find(&t, k);
            check((f >= 0) == (ri >= 0), "find");
            if (f >= 0)
                check(t.val[f] == ref_v[ri], "val");
            false_pos_checked++;
        }
        check(t.n == (size_t)ref_n, "size");
    }
    /* SWAR sanity: a known group */
    uint8_t demo[8] = {0x80, 0x11, 0x22, 0x11, 0xFE, 0x33, 0x11, 0x80};
    uint64_t m = match_byte(load_group(demo), 0x11);
    int hits = 0;
    for (int i = 0; i < 8; i++)
        if ((m >> (i * 8 + 7) & 1) && demo[i] == 0x11)
            hits++;
    check(hits == 3, "swar exact matches");
    size_t empt = 0, del = 0, full = 0;
    for (size_t i = 0; i < t.ngroups * G; i++)
        t.ctrl[i] == CTRL_EMPTY ? empt++ : t.ctrl[i] == CTRL_DEL ? del++ : full++;
    check(full == t.n && del == t.deleted, "ctrl accounting");
    printf("n=%zu groups=%zu slots=%zu load=%.4f\n", t.n, t.ngroups, t.ngroups * G, (double)t.n / (double)(t.ngroups * G));
    printf("ctrl: full=%zu empty=%zu deleted=%zu rehashes=%ld\n", full, empt, del, t.rehashes);
    printf("lookups=%ld avg groups probed=%.4f\n", false_pos_checked, (double)t.group_probes / (double)t.ops);
    tab_free(&t);
    return 0;
}
