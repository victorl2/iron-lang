/*
 * title: Two-choice chained hash table versus one-choice
 * topic: data_structures
 * covers: power of two choices, shorter-chain insertion, lookup in two buckets, delete, maximum chain length, load balance histogram
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
    Node **b;
    size_t nb, n;
    int choices;
    long probes, ops;
} Tab;

static size_t bucket(const Tab *t, uint32_t k, int which) {
    return (which ? mix32(k * 0x9E3779B1u + 0x1234567u) : mix32(k)) % t->nb;
}
static int chain_len(const Node *p) {
    int c = 0;
    for (; p; p = p->next)
        c++;
    return c;
}

static void tab_init(Tab *t, size_t nb, int choices) {
    t->b = calloc(nb, sizeof(Node *));
    t->nb = nb;
    t->n = 0;
    t->choices = choices;
    t->probes = t->ops = 0;
}

static Node *tab_get(Tab *t, uint32_t k) {
    t->ops++;
    for (int w = 0; w < t->choices; w++)
        for (Node *p = t->b[bucket(t, k, w)]; p; p = p->next) {
            t->probes++;
            if (p->key == k)
                return p;
        }
    return NULL;
}

static int tab_put(Tab *t, uint32_t k, int v) {
    Node *p = tab_get(t, k);
    if (p) {
        p->val = v;
        return 0;
    }
    size_t dst = bucket(t, k, 0);
    if (t->choices == 2) {
        size_t alt = bucket(t, k, 1);
        if (chain_len(t->b[alt]) < chain_len(t->b[dst]))
            dst = alt;
    }
    p = malloc(sizeof *p);
    p->key = k;
    p->val = v;
    p->next = t->b[dst];
    t->b[dst] = p;
    t->n++;
    return 1;
}

static int tab_del(Tab *t, uint32_t k) {
    for (int w = 0; w < t->choices; w++)
        for (Node **pp = &t->b[bucket(t, k, w)]; *pp; pp = &(*pp)->next)
            if ((*pp)->key == k) {
                Node *x = *pp;
                *pp = x->next;
                free(x);
                t->n--;
                return 1;
            }
    return 0;
}

static void tab_free(Tab *t) {
    for (size_t i = 0; i < t->nb; i++)
        for (Node *p = t->b[i]; p;) {
            Node *nx = p->next;
            free(p);
            p = nx;
        }
    free(t->b);
}

int main(void) {
    Tab t1, t2;
    tab_init(&t1, 2048, 1);
    tab_init(&t2, 2048, 2);
    /* phase 1: heavy load with mixed operations, cross-checked against the reference */
    for (int step = 0; step < 40000; step++) {
        uint32_t k = (uint32_t)(rnd() % 9000);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 6) {
            int v = (int)(rnd() & 0xffff);
            int a = tab_put(&t1, k, v), b = tab_put(&t2, k, v);
            check(a == (ri < 0) && b == a, "put");
            ref_put(k, v);
        } else if (op < 8) {
            int a = tab_del(&t1, k), b = tab_del(&t2, k);
            check(a == (ri >= 0) && b == a, "del");
            ref_del(k);
        } else {
            Node *a = tab_get(&t1, k), *b = tab_get(&t2, k);
            check((a != NULL) == (ri >= 0) && (b != NULL) == (ri >= 0), "get");
            if (a)
                check(a->val == ref_v[ri] && b->val == ref_v[ri], "value");
        }
        check(t1.n == (size_t)ref_n && t2.n == (size_t)ref_n, "size");
    }
    Tab *ts[2] = {&t1, &t2};
    const char *names[2] = {"one choice ", "two choices"};
    for (int w = 0; w < 2; w++) {
        int hist[8] = {0}, mx = 0;
        for (size_t i = 0; i < ts[w]->nb; i++) {
            int c = chain_len(ts[w]->b[i]);
            hist[c < 7 ? c : 7]++;
            if (c > mx)
                mx = c;
        }
        printf("%s n=%zu buckets=%zu max chain=%d avg probes/op=%.4f\n", names[w], ts[w]->n, ts[w]->nb, mx,
               (double)ts[w]->probes / (double)ts[w]->ops);
        printf("  chain length histogram:");
        for (int c = 0; c < 8; c++)
            printf(" %d%s:%d", c, c == 7 ? "+" : "", hist[c]);
        printf("\n");
    }
    tab_free(&t1);
    tab_free(&t2);
    /* phase 2: pure insertion of 4n keys into n buckets, the classic balls-into-bins comparison */
    for (int w = 0; w < 2; w++) {
        Tab t;
        tab_init(&t, 4096, w + 1);
        for (uint32_t k = 0; k < 4096u * 4; k++)
            tab_put(&t, k * 2654435761u + 5u, 1);
        int mx = 0;
        for (size_t i = 0; i < t.nb; i++)
            if (chain_len(t.b[i]) > mx)
                mx = chain_len(t.b[i]);
        printf("4 keys per bucket, %s: max chain=%d (mean 4)\n", names[w], mx);
        tab_free(&t);
    }
    return 0;
}
