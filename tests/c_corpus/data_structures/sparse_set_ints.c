/*
 * title: Sparse set for integer universes
 * topic: data_structures
 * covers: sparse set, dense and sparse arrays, O(1) clear, membership without initialization, iteration order
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x13198A2E03707344ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 25); }

typedef struct { unsigned *dense, *sparse; unsigned n, universe; } SSet;

static void ss_init(SSet *s, unsigned universe) {
    s->dense = malloc(universe * sizeof(unsigned));
    s->sparse = malloc(universe * sizeof(unsigned)); /* contents are meaningless until validated against dense[] */
    CHECK(s->dense && s->sparse);
    for (unsigned i = 0; i < universe; i++) s->sparse[i] = (i * 2654435761u) ^ 0xDEADBEEFu; /* arbitrary junk: membership must not depend on it */
    s->n = 0; s->universe = universe;
}
static void ss_free(SSet *s) { free(s->dense); free(s->sparse); }
static int ss_has(const SSet *s, unsigned x) { return x < s->universe && s->sparse[x] < s->n && s->dense[s->sparse[x]] == x; }
static int ss_add(SSet *s, unsigned x) { CHECK(x < s->universe); if (ss_has(s, x)) return 0; s->dense[s->n] = x; s->sparse[x] = s->n++; return 1; }
static int ss_del(SSet *s, unsigned x) {
    if (!ss_has(s, x)) return 0;
    unsigned last = s->dense[--s->n];
    s->dense[s->sparse[x]] = last; s->sparse[last] = s->sparse[x];
    return 1;
}
static void ss_clear(SSet *s) { s->n = 0; }
static void ss_union(SSet *a, const SSet *b) { for (unsigned i = 0; i < b->n; i++) ss_add(a, b->dense[i]); }
static void ss_intersect(SSet *a, const SSet *b) { for (unsigned i = 0; i < a->n;) if (!ss_has(b, a->dense[i])) ss_del(a, a->dense[i]); else i++; }

#define U 512
int main(void) {
    SSet a, b; ss_init(&a, U); ss_init(&b, U);
    unsigned char ma[U] = {0}, mb[U] = {0};
    long adds = 0, dels = 0, has = 0, clears = 0, unions = 0, inters = 0;
    for (int step = 0; step < 30000; step++) {
        unsigned op = rnd() % 1000;
        unsigned x = rnd() % U;
        SSet *s = (rnd() & 1) ? &a : &b; unsigned char *m = (s == &a) ? ma : mb;
        if (op < 400) { int r = ss_add(s, x); CHECK(r == !m[x]); m[x] = 1; adds++; }
        else if (op < 700) { int r = ss_del(s, x); CHECK(r == m[x]); m[x] = 0; dels++; }
        else if (op < 970) { CHECK(ss_has(s, x) == m[x]); has++; }
        else if (op < 980) { ss_clear(s); memset(m, 0, U); clears++; }
        else if (op < 990) { ss_union(&a, &b); for (int i = 0; i < U; i++) ma[i] |= mb[i]; unions++; }
        else { ss_intersect(&a, &b); for (int i = 0; i < U; i++) ma[i] &= mb[i]; inters++; }
        if (step % 10 == 0) {
            unsigned ca = 0, cb = 0;
            for (int i = 0; i < U; i++) { CHECK(ss_has(&a, (unsigned)i) == ma[i] && ss_has(&b, (unsigned)i) == mb[i]); ca += ma[i]; cb += mb[i]; }
            CHECK(a.n == ca && b.n == cb);
            for (unsigned i = 0; i < a.n; i++) CHECK(ma[a.dense[i]]);
        }
    }
    printf("add=%ld del=%ld has=%ld clear=%ld union=%ld intersect=%ld\n", adds, dels, has, clears, unions, inters);
    printf("|a|=%u |b|=%u\n", a.n, b.n);
    /* dense iteration order is insertion order modulo swap-removals; show that it is not sorted */
    unsigned sorted = 1; for (unsigned i = 1; i < a.n; i++) if (a.dense[i - 1] > a.dense[i]) sorted = 0;
    printf("a dense first:"); for (unsigned i = 0; i < a.n && i < 8; i++) printf(" %u", a.dense[i]);
    printf("\ndense order sorted? %s\n", sorted ? "yes" : "no");
    ss_free(&a); ss_free(&b);
    return 0;
}
