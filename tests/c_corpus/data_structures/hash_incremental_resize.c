/*
 * title: Incremental resizing hash table (two generations)
 * topic: data_structures
 * covers: incremental rehash, old and new table, migration cursor, bounded work per operation, chaining
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
typedef struct Node {
    uint32_t key;
    int val;
    struct Node *next;
} Node;
typedef struct {
    Node **b[2];
    size_t nb[2];
    size_t n;
    int rehashing;   /* 1 while table 0 is draining into table 1 */
    size_t cursor;   /* next bucket of table 0 to migrate */
    long starts, migrated_nodes, max_step_work, total_steps_in_rehash;
} Dict;

static void dict_init(Dict *d) {
    memset(d, 0, sizeof *d);
    d->nb[0] = 4;
    d->b[0] = calloc(4, sizeof(Node *));
}

static void rehash_step(Dict *d, int buckets, long *work) {
    if (!d->rehashing)
        return;
    d->total_steps_in_rehash++;
    while (buckets-- > 0 && d->cursor < d->nb[0]) {
        Node *p = d->b[0][d->cursor];
        d->b[0][d->cursor] = NULL;
        while (p) {
            Node *nx = p->next;
            size_t j = mix32(p->key) % d->nb[1];
            p->next = d->b[1][j];
            d->b[1][j] = p;
            p = nx;
            d->migrated_nodes++;
            (*work)++;
        }
        d->cursor++;
        (*work)++;
    }
    if (d->cursor >= d->nb[0]) {
        free(d->b[0]);
        d->b[0] = d->b[1];
        d->nb[0] = d->nb[1];
        d->b[1] = NULL;
        d->nb[1] = 0;
        d->rehashing = 0;
        d->cursor = 0;
    }
}

static Node *dict_find(Dict *d, uint32_t k) {
    for (int t = 0; t <= d->rehashing; t++)
        for (Node *p = d->b[t][mix32(k) % d->nb[t]]; p; p = p->next)
            if (p->key == k)
                return p;
    return NULL;
}

static void dict_put(Dict *d, uint32_t k, int v) {
    long work = 0;
    rehash_step(d, 2, &work);
    Node *p = dict_find(d, k);
    if (p) {
        p->val = v;
    } else {
        if (!d->rehashing && d->n >= d->nb[0]) { /* load factor 1: start growing */
            d->nb[1] = d->nb[0] * 2;
            d->b[1] = calloc(d->nb[1], sizeof(Node *));
            d->rehashing = 1;
            d->cursor = 0;
            d->starts++;
        }
        int t = d->rehashing ? 1 : 0; /* new keys always go to the new table */
        size_t j = mix32(k) % d->nb[t];
        p = malloc(sizeof *p);
        p->key = k;
        p->val = v;
        p->next = d->b[t][j];
        d->b[t][j] = p;
        d->n++;
        work++;
    }
    if (work > d->max_step_work)
        d->max_step_work = work;
}

static int dict_del(Dict *d, uint32_t k) {
    long work = 0;
    rehash_step(d, 2, &work);
    for (int t = 0; t <= d->rehashing; t++) {
        Node **pp = &d->b[t][mix32(k) % d->nb[t]];
        for (; *pp; pp = &(*pp)->next)
            if ((*pp)->key == k) {
                Node *x = *pp;
                *pp = x->next;
                free(x);
                d->n--;
                return 1;
            }
    }
    return 0;
}

static void dict_free(Dict *d) {
    for (int t = 0; t < 2; t++) {
        if (!d->b[t])
            continue;
        for (size_t i = 0; i < d->nb[t]; i++)
            for (Node *p = d->b[t][i]; p;) {
                Node *nx = p->next;
                free(p);
                p = nx;
            }
        free(d->b[t]);
    }
}

int main(void) {
    Dict d;
    dict_init(&d);
    long overlap_lookups = 0;
    for (int step = 0; step < 40000; step++) {
        uint32_t k = (uint32_t)(rnd() % 5000);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 6) {
            int v = (int)(rnd() & 0xffff);
            dict_put(&d, k, v);
            ref_put(k, v);
        } else if (op < 8) {
            check(dict_del(&d, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            long w = 0;
            rehash_step(&d, 1, &w);
            overlap_lookups += d.rehashing;
            Node *p = dict_find(&d, k);
            check((p != NULL) == (ri >= 0), "find");
            if (p)
                check(p->val == ref_v[ri], "val");
        }
        check(d.n == (size_t)ref_n, "size");
    }
    for (int i = 0; i < ref_n; i++) {
        Node *p = dict_find(&d, ref_k[i]);
        check(p && p->val == ref_v[i], "sweep");
    }
    printf("n=%zu buckets=%zu rehashing=%d\n", d.n, d.nb[0], d.rehashing);
    printf("growth cycles started=%ld nodes migrated=%ld\n", d.starts, d.migrated_nodes);
    printf("max work in one operation=%ld (a full rehash would touch up to %zu)\n", d.max_step_work, d.nb[0] + d.n);
    printf("lookups served while two tables were live=%ld\n", overlap_lookups);
    dict_free(&d);
    return 0;
}
