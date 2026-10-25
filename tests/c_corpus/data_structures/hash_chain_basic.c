/*
 * title: Separate chaining hash map with resize
 * topic: data_structures
 * covers: separate chaining, load factor, rehash, chain length histogram, reference model
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct Node {
    uint32_t key;
    int val;
    struct Node *next;
} Node;

typedef struct {
    Node **b;
    size_t nb, n;
    long resizes, probes, lookups;
} Map;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void map_init(Map *m, size_t nb) {
    m->b = calloc(nb, sizeof(Node *));
    m->nb = nb;
    m->n = 0;
    m->resizes = m->probes = m->lookups = 0;
}

static void map_free(Map *m) {
    for (size_t i = 0; i < m->nb; i++)
        for (Node *p = m->b[i]; p;) {
            Node *nx = p->next;
            free(p);
            p = nx;
        }
    free(m->b);
}

static void map_grow(Map *m) {
    size_t nn = m->nb * 2;
    Node **nb = calloc(nn, sizeof(Node *));
    for (size_t i = 0; i < m->nb; i++)
        for (Node *p = m->b[i]; p;) {
            Node *nx = p->next;
            size_t j = hash32(p->key) % nn;
            p->next = nb[j];
            nb[j] = p;
            p = nx;
        }
    free(m->b);
    m->b = nb;
    m->nb = nn;
    m->resizes++;
}

static Node *map_find(Map *m, uint32_t k) {
    m->lookups++;
    for (Node *p = m->b[hash32(k) % m->nb]; p; p = p->next) {
        m->probes++;
        if (p->key == k)
            return p;
    }
    return NULL;
}

static int map_put(Map *m, uint32_t k, int v) {
    Node *p = map_find(m, k);
    if (p) {
        p->val = v;
        return 0;
    }
    if (m->n * 4 >= m->nb * 3) /* load factor 0.75 */
        map_grow(m);
    size_t j = hash32(k) % m->nb;
    p = malloc(sizeof *p);
    p->key = k;
    p->val = v;
    p->next = m->b[j];
    m->b[j] = p;
    m->n++;
    return 1;
}

static int map_del(Map *m, uint32_t k) {
    Node **pp = &m->b[hash32(k) % m->nb];
    while (*pp) {
        if ((*pp)->key == k) {
            Node *d = *pp;
            *pp = d->next;
            free(d);
            m->n--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

enum { RN = 4096 };
static uint32_t rk[RN];
static int rv[RN];
static int rn;

static int ref_find(uint32_t k) {
    for (int i = 0; i < rn; i++)
        if (rk[i] == k)
            return i;
    return -1;
}

int main(void) {
    Map m;
    map_init(&m, 8);
    long ins = 0, upd = 0, del = 0, hit = 0, miss = 0;
    for (int step = 0; step < 20000; step++) {
        uint32_t k = (uint32_t)(rnd() % 1500);
        int op = (int)(rnd() % 10);
        int ri = ref_find(k);
        if (op < 5) {
            int v = (int)(rnd() & 0xffff);
            int fresh = map_put(&m, k, v);
            check(fresh == (ri < 0), "put freshness");
            if (ri < 0) {
                check(rn < RN, "ref capacity");
                rk[rn] = k;
                rv[rn++] = v;
                ins++;
            } else {
                rv[ri] = v;
                upd++;
            }
        } else if (op < 7) {
            int r = map_del(&m, k);
            check(r == (ri >= 0), "del result");
            if (ri >= 0) {
                rk[ri] = rk[rn - 1];
                rv[ri] = rv[rn - 1];
                rn--;
                del++;
            }
        } else {
            Node *p = map_find(&m, k);
            check((p != NULL) == (ri >= 0), "find presence");
            if (p) {
                check(p->val == rv[ri], "find value");
                hit++;
            } else
                miss++;
        }
        check(m.n == (size_t)rn, "size");
    }
    int hist[8] = {0};
    size_t maxlen = 0;
    for (size_t i = 0; i < m.nb; i++) {
        size_t len = 0;
        for (Node *p = m.b[i]; p; p = p->next)
            len++;
        if (len > maxlen)
            maxlen = len;
        hist[len < 7 ? len : 7]++;
    }
    printf("inserts=%ld updates=%ld deletes=%ld hits=%ld misses=%ld\n", ins, upd, del, hit, miss);
    printf("size=%zu buckets=%zu resizes=%ld load=%.4f\n", m.n, m.nb, m.resizes, (double)m.n / (double)m.nb);
    printf("avg probes per lookup=%.4f maxchain=%zu\n", (double)m.probes / (double)m.lookups, maxlen);
    for (int i = 0; i < 8; i++)
        printf("chain len %d%s: %d buckets\n", i, i == 7 ? "+" : "", hist[i]);
    map_free(&m);
    return 0;
}
