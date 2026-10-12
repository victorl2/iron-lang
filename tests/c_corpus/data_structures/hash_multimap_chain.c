/*
 * title: Multimap with per-key value lists
 * topic: data_structures
 * covers: multimap, duplicate keys, value lists in insertion order, remove one vs remove all, count and range queries
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
typedef struct Val {
    int v;
    struct Val *next;
} Val;
typedef struct Ent {
    uint32_t key;
    Val *head, *tail;
    int count;
    struct Ent *next;
} Ent;
typedef struct {
    Ent **b;
    size_t nb, keys, pairs;
} MM;

static void mm_init(MM *m, size_t nb) {
    m->b = calloc(nb, sizeof(Ent *));
    m->nb = nb;
    m->keys = m->pairs = 0;
}

static Ent *mm_ent(MM *m, uint32_t k, int create) {
    Ent **pp = &m->b[mix32(k) % m->nb];
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->key == k)
            return *pp;
    if (!create)
        return NULL;
    Ent *e = calloc(1, sizeof *e);
    e->key = k;
    *pp = e;
    m->keys++;
    return e;
}

static void mm_add(MM *m, uint32_t k, int v) {
    Ent *e = mm_ent(m, k, 1);
    Val *x = malloc(sizeof *x);
    x->v = v;
    x->next = NULL;
    if (e->tail)
        e->tail->next = x;
    else
        e->head = x;
    e->tail = x;
    e->count++;
    m->pairs++;
}

static void drop_ent(MM *m, uint32_t k) {
    Ent **pp = &m->b[mix32(k) % m->nb];
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->key == k) {
            Ent *e = *pp;
            *pp = e->next;
            free(e);
            m->keys--;
            return;
        }
}

static int mm_remove_all(MM *m, uint32_t k) {
    Ent *e = mm_ent(m, k, 0);
    if (!e)
        return 0;
    int c = e->count;
    for (Val *x = e->head; x;) {
        Val *nx = x->next;
        free(x);
        x = nx;
    }
    m->pairs -= (size_t)c;
    drop_ent(m, k);
    return c;
}

static int mm_remove_one(MM *m, uint32_t k, int v) { /* first matching value */
    Ent *e = mm_ent(m, k, 0);
    if (!e)
        return 0;
    Val **pp = &e->head, *prev = NULL;
    for (; *pp; prev = *pp, pp = &(*pp)->next)
        if ((*pp)->v == v) {
            Val *x = *pp;
            *pp = x->next;
            if (e->tail == x)
                e->tail = prev;
            free(x);
            e->count--;
            m->pairs--;
            if (!e->count)
                drop_ent(m, k);
            return 1;
        }
    return 0;
}

static void mm_free(MM *m) {
    for (size_t i = 0; i < m->nb; i++)
        for (Ent *e = m->b[i]; e;) {
            Ent *nx = e->next;
            for (Val *x = e->head; x;) {
                Val *nv = x->next;
                free(x);
                x = nv;
            }
            free(e);
            e = nx;
        }
    free(m->b);
}

/* reference: flat array of (key,val) pairs in insertion order */
enum { MAXP = 8192 };
static uint32_t pk[MAXP];
static int pv[MAXP], np;

int main(void) {
    MM m;
    mm_init(&m, 64);
    long adds = 0, rem1 = 0, remall = 0, queries = 0;
    for (int step = 0; step < 20000; step++) {
        uint32_t k = (uint32_t)(rnd() % 200);
        int op = (int)(rnd() % 10);
        if (op < 5 && np < MAXP) {
            int v = (int)(rnd() % 50);
            mm_add(&m, k, v);
            pk[np] = k;
            pv[np++] = v;
            adds++;
        } else if (op < 8) {
            int v = (int)(rnd() % 50), found = 0;
            for (int i = 0; i < np; i++)
                if (pk[i] == k && pv[i] == v) {
                    memmove(&pk[i], &pk[i + 1], (size_t)(np - i - 1) * sizeof pk[0]);
                    memmove(&pv[i], &pv[i + 1], (size_t)(np - i - 1) * sizeof pv[0]);
                    np--;
                    found = 1;
                    break;
                }
            check(mm_remove_one(&m, k, v) == found, "remove one");
            rem1 += found;
        } else if (op < 9) {
            int c = 0, w = 0;
            for (int i = 0; i < np; i++)
                if (pk[i] == k)
                    c++;
                else {
                    pk[w] = pk[i];
                    pv[w++] = pv[i];
                }
            np = w;
            check(mm_remove_all(&m, k) == c, "remove all");
            remall += c;
        } else {
            Ent *e = mm_ent(&m, k, 0);
            int idx = 0;
            for (int i = 0; i < np; i++)
                if (pk[i] == k) {
                    check(e != NULL, "entry exists");
                    Val *x = e->head;
                    for (int j = 0; j < idx; j++)
                        x = x ? x->next : NULL;
                    check(x && x->v == pv[i], "value order");
                    idx++;
                }
            check((e ? e->count : 0) == idx, "count");
            queries++;
        }
        check(m.pairs == (size_t)np, "pair count");
    }
    int maxc = 0;
    uint32_t maxk = 0;
    for (size_t i = 0; i < m.nb; i++)
        for (Ent *e = m.b[i]; e; e = e->next)
            if (e->count > maxc || (e->count == maxc && e->key < maxk)) {
                maxc = e->count;
                maxk = e->key;
            }
    printf("adds=%ld remove-one=%ld remove-all=%ld queries=%ld\n", adds, rem1, remall, queries);
    printf("distinct keys=%zu pairs=%zu busiest key=%u (%d values)\n", m.keys, m.pairs, maxk, maxc);
    Ent *e = mm_ent(&m, maxk, 0);
    printf("its values:");
    int shown = 0;
    for (Val *x = e->head; x && shown < 12; x = x->next, shown++)
        printf(" %d", x->v);
    printf("\n");
    mm_free(&m);
    return 0;
}
