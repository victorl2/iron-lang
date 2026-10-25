/*
 * title: Chained hash table with move-to-front and transpose heuristics
 * topic: data_structures
 * covers: self-organizing chains, move-to-front, transpose, skewed access, probe counting, policy comparison
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
typedef enum { PLAIN, MTF, TRANSPOSE } Policy;
typedef struct {
    Node **b;
    size_t nb;
    Policy pol;
    long probes, lookups;
} Tab;

static const char *pol_name(Policy p) { return p == PLAIN ? "plain" : p == MTF ? "move-to-front" : "transpose"; }

static void tab_init(Tab *t, size_t nb, Policy p) {
    t->b = calloc(nb, sizeof(Node *));
    t->nb = nb;
    t->pol = p;
    t->probes = t->lookups = 0;
}
static void tab_free(Tab *t) {
    for (size_t i = 0; i < t->nb; i++)
        for (Node *n = t->b[i]; n;) {
            Node *nx = n->next;
            free(n);
            n = nx;
        }
    free(t->b);
}

static Node *tab_get(Tab *t, uint32_t k) {
    Node **head = &t->b[mix32(k) % t->nb];
    Node *prev = NULL, *pprev = NULL;
    t->lookups++;
    for (Node *n = *head; n; pprev = prev, prev = n, n = n->next) {
        t->probes++;
        if (n->key != k)
            continue;
        if (t->pol == MTF && prev) {
            prev->next = n->next;
            n->next = *head;
            *head = n;
        } else if (t->pol == TRANSPOSE && prev) {
            /* swap n with its predecessor */
            prev->next = n->next;
            n->next = prev;
            if (pprev)
                pprev->next = n;
            else
                *head = n;
        }
        return n;
    }
    return NULL;
}

static void tab_put(Tab *t, uint32_t k, int v) {
    Node *n = tab_get(t, k);
    if (n) {
        n->val = v;
        return;
    }
    Node **head = &t->b[mix32(k) % t->nb];
    n = malloc(sizeof *n);
    n->key = k;
    n->val = v;
    n->next = NULL;
    Node **pp = head; /* append at the tail so that initial order is arrival order */
    while (*pp)
        pp = &(*pp)->next;
    *pp = n;
}

int main(void) {
    enum { KEYS = 600, ACC = 60000 };
    /* skewed key popularity: rank r drawn with weight ~ 1/(r+1) via rejection on a harmonic table */
    static uint32_t cdf[KEYS];
    uint32_t acc = 0;
    for (int i = 0; i < KEYS; i++) {
        acc += 100000u / (uint32_t)(i + 1);
        cdf[i] = acc;
    }
    static uint32_t trace[ACC];
    for (int i = 0; i < ACC; i++) {
        uint32_t r = (uint32_t)(rnd() % acc);
        int lo = 0, hi = KEYS - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (cdf[mid] > r)
                hi = mid;
            else
                lo = mid + 1;
        }
        trace[i] = (uint32_t)lo * 7919u + 13u; /* rank to key */
    }
    for (int p = PLAIN; p <= TRANSPOSE; p++) {
        Tab t;
        tab_init(&t, 16, (Policy)p); /* deliberately small: long chains make the heuristic matter */
        static int order[KEYS];
        for (int i = 0; i < KEYS; i++)
            order[i] = i;
        rs = 4242; /* same shuffle for every policy */
        for (int i = KEYS - 1; i > 0; i--) {
            int j = (int)(rnd() % (uint64_t)(i + 1)), tmp = order[i];
            order[i] = order[j];
            order[j] = tmp;
        }
        for (int i = 0; i < KEYS; i++)
            tab_put(&t, (uint32_t)order[i] * 7919u + 13u, order[i]);
        t.probes = t.lookups = 0;
        long hits = 0, sum = 0;
        for (int i = 0; i < ACC; i++) {
            Node *n = tab_get(&t, trace[i]);
            check(n != NULL, "key present");
            int rank = (int)((trace[i] - 13u) / 7919u);
            check(n->val == rank, "value matches reference");
            hits++;
            sum += n->val;
        }
        for (int i = 0; i < KEYS; i++) /* nothing lost or duplicated */
            check(tab_get(&t, (uint32_t)i * 7919u + 13u) != NULL, "still present");
        long total = 0;
        for (size_t i = 0; i < t.nb; i++)
            for (Node *n = t.b[i]; n; n = n->next)
                total++;
        check(total == KEYS, "no duplication");
        printf("%-14s hits=%ld value sum=%ld avg probes per lookup=%.4f\n", pol_name((Policy)p), hits, sum, (double)t.probes / (double)t.lookups);
        tab_free(&t);
    }
    return 0;
}
