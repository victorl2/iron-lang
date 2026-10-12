/*
 * title: Litwin linear hashing with split pointer
 * topic: data_structures
 * covers: linear hashing, split pointer, level, bucket splitting and merging, overflow chains, controlled load
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
    size_t cap_buckets;
    size_t n0;      /* initial bucket count */
    unsigned level; /* current round */
    size_t split;   /* next bucket to split */
    size_t n;
    long splits, merges, maxbuckets;
} LH;

static size_t nbuckets(const LH *t) { return (t->n0 << t->level) + t->split; }

static size_t addr(const LH *t, uint32_t k) {
    uint32_t h = mix32(k);
    size_t a = h % (t->n0 << t->level);
    if (a < t->split)
        a = h % (t->n0 << (t->level + 1));
    return a;
}

static void lh_init(LH *t) {
    memset(t, 0, sizeof *t);
    t->n0 = 2;
    t->cap_buckets = 64;
    t->b = calloc(t->cap_buckets, sizeof(Node *));
}

static void ensure(LH *t, size_t nb) {
    while (nb > t->cap_buckets) {
        size_t nc = t->cap_buckets * 2;
        t->b = realloc(t->b, nc * sizeof(Node *));
        memset(t->b + t->cap_buckets, 0, (nc - t->cap_buckets) * sizeof(Node *));
        t->cap_buckets = nc;
    }
}

static void do_split(LH *t) {
    size_t old = t->split, nw = (t->n0 << t->level) + t->split;
    ensure(t, nw + 1);
    t->split++;
    if (t->split == (t->n0 << t->level)) {
        t->level++;
        t->split = 0;
    }
    Node *p = t->b[old];
    t->b[old] = NULL;
    while (p) {
        Node *nx = p->next;
        size_t a = addr(t, p->key);
        check(a == old || a == nw, "split target");
        p->next = t->b[a];
        t->b[a] = p;
        p = nx;
    }
    t->splits++;
    if ((long)nbuckets(t) > t->maxbuckets)
        t->maxbuckets = (long)nbuckets(t);
}

static void do_merge(LH *t) {
    if (t->split == 0) {
        check(t->level > 0, "level>0");
        t->level--;
        t->split = t->n0 << t->level;
    }
    t->split--;
    size_t keep = t->split, gone = (t->n0 << t->level) + t->split;
    Node *p = t->b[gone];
    t->b[gone] = NULL;
    while (p) {
        Node *nx = p->next;
        p->next = t->b[keep];
        t->b[keep] = p;
        p = nx;
    }
    t->merges++;
}

static Node *lh_find(LH *t, uint32_t k) {
    for (Node *p = t->b[addr(t, k)]; p; p = p->next)
        if (p->key == k)
            return p;
    return NULL;
}

static int lh_put(LH *t, uint32_t k, int v) {
    Node *p = lh_find(t, k);
    if (p) {
        p->val = v;
        return 0;
    }
    size_t a = addr(t, k);
    p = malloc(sizeof *p);
    p->key = k;
    p->val = v;
    p->next = t->b[a];
    t->b[a] = p;
    t->n++;
    while (t->n * 10 > nbuckets(t) * 20) /* split until load <= 2.0 per bucket */
        do_split(t);
    return 1;
}

static int lh_del(LH *t, uint32_t k) {
    Node **pp = &t->b[addr(t, k)];
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->key == k) {
            Node *x = *pp;
            *pp = x->next;
            free(x);
            t->n--;
            while (nbuckets(t) > t->n0 && t->n * 10 < nbuckets(t) * 8)
                do_merge(t); /* merge until load >= 0.8 */
            return 1;
        }
    return 0;
}

int main(void) {
    LH t;
    lh_init(&t);
    long trace_buckets[8], tr = 0;
    for (int step = 0; step < 36000; step++) {
        uint32_t k = (uint32_t)(rnd() % 2500);
        int ri = ref_find(k);
        /* alternate a growth phase and a shrink phase */
        int grow_phase = (step / 6000) % 2 == 0;
        int op = (int)(rnd() % 10);
        if (op < (grow_phase ? 8 : 1)) {
            int v = (int)(rnd() & 0xffff);
            check(lh_put(&t, k, v) == (ri < 0), "put");
            ref_put(k, v);
        } else if (op < 9) {
            check(lh_del(&t, k) == (ri >= 0), "del");
            ref_del(k);
        } else {
            Node *p = lh_find(&t, k);
            check((p != NULL) == (ri >= 0), "find");
            if (p)
                check(p->val == ref_v[ri], "val");
        }
        check(t.n == (size_t)ref_n, "size");
        if (step % 4500 == 4499 && tr < 8)
            trace_buckets[tr++] = (long)nbuckets(&t);
    }
    size_t total = 0, maxc = 0;
    for (size_t i = 0; i < nbuckets(&t); i++) {
        size_t c = 0;
        for (Node *p = t.b[i]; p; p = p->next) {
            check(addr(&t, p->key) == i, "node in right bucket");
            c++;
        }
        total += c;
        if (c > maxc)
            maxc = c;
    }
    check(total == t.n, "count");
    printf("n=%zu buckets=%zu level=%u split=%zu\n", t.n, nbuckets(&t), t.level, t.split);
    printf("splits=%ld merges=%ld peak buckets=%ld longest chain=%zu\n", t.splits, t.merges, t.maxbuckets, maxc);
    printf("bucket count trace:");
    for (long i = 0; i < tr; i++)
        printf(" %ld", trace_buckets[i]);
    printf("\n");
    for (size_t i = 0; i < nbuckets(&t); i++)
        for (Node *p = t.b[i]; p;) {
            Node *nx = p->next;
            free(p);
            p = nx;
        }
    free(t.b);
    return 0;
}
