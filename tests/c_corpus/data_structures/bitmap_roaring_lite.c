/*
 * title: Roaring-style compressed bitmap with array and bitmap containers
 * topic: data_structures
 * covers: roaring bitmap, chunking by high 16 bits, sorted array container, 1024-word bitmap container, 4096 threshold conversion, and/or/andnot, iteration, oracle bitmap
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHUNKS 16
#define UNIVERSE (CHUNKS * 65536u)
#define ARRAY_MAX 4096

static unsigned long long rs = 0x40A216ULL * 0x9E3779B9ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int is_bitmap; int card; uint16_t *arr; int cap; uint64_t *bits; } Cont;
typedef struct { Cont *c[CHUNKS]; } Roar;

static int pop64(uint64_t x) { int c = 0; while (x) { x &= x - 1; c++; } return c; }

static Cont *cont_new(void) { return calloc(1, sizeof(Cont)); }
static void cont_free(Cont *c) { if (!c) return; free(c->arr); free(c->bits); free(c); }
static void to_bitmap(Cont *c) {
    if (c->is_bitmap) return;
    c->bits = calloc(1024, sizeof(uint64_t));
    for (int i = 0; i < c->card; i++) c->bits[c->arr[i] >> 6] |= (uint64_t)1 << (c->arr[i] & 63);
    free(c->arr); c->arr = NULL; c->cap = 0; c->is_bitmap = 1;
}
static void to_array(Cont *c) {
    if (!c->is_bitmap) return;
    uint16_t *a = malloc((size_t)(c->card ? c->card : 1) * sizeof(uint16_t)); int n = 0;
    for (int w = 0; w < 1024; w++) for (uint64_t x = c->bits[w]; x; x &= x - 1) {
        int b = 0; while (!((x >> b) & 1u)) b++;
        a[n++] = (uint16_t)(w * 64 + b);
    }
    check(n == c->card, "bitmap cardinality");
    free(c->bits); c->bits = NULL; c->arr = a; c->cap = c->card ? c->card : 1; c->is_bitmap = 0;
}
static int cont_has(const Cont *c, uint16_t v) {
    if (c->is_bitmap) return (int)((c->bits[v >> 6] >> (v & 63)) & 1u);
    int lo = 0, hi = c->card;
    while (lo < hi) { int m = (lo + hi) / 2; if (c->arr[m] < v) lo = m + 1; else hi = m; }
    return lo < c->card && c->arr[lo] == v;
}
static void cont_add(Cont *c, uint16_t v) {
    if (cont_has(c, v)) return;
    if (c->is_bitmap) { c->bits[v >> 6] |= (uint64_t)1 << (v & 63); c->card++; return; }
    if (c->card == ARRAY_MAX) { to_bitmap(c); c->bits[v >> 6] |= (uint64_t)1 << (v & 63); c->card++; return; }
    if (c->card == c->cap) { c->cap = c->cap ? c->cap * 2 : 4; c->arr = realloc(c->arr, (size_t)c->cap * sizeof(uint16_t)); }
    int lo = 0, hi = c->card;
    while (lo < hi) { int m = (lo + hi) / 2; if (c->arr[m] < v) lo = m + 1; else hi = m; }
    memmove(c->arr + lo + 1, c->arr + lo, (size_t)(c->card - lo) * sizeof(uint16_t));
    c->arr[lo] = v; c->card++;
}
static void cont_del(Cont *c, uint16_t v) {
    if (!cont_has(c, v)) return;
    if (c->is_bitmap) {
        c->bits[v >> 6] &= ~((uint64_t)1 << (v & 63)); c->card--;
        if (c->card <= ARRAY_MAX) to_array(c);
        return;
    }
    int lo = 0; while (c->arr[lo] != v) lo++;
    memmove(c->arr + lo, c->arr + lo + 1, (size_t)(c->card - lo - 1) * sizeof(uint16_t)); c->card--;
}
static void as_words(const Cont *c, uint64_t *out) {
    if (c->is_bitmap) { memcpy(out, c->bits, 1024 * sizeof(uint64_t)); return; }
    memset(out, 0, 1024 * sizeof(uint64_t));
    for (int i = 0; i < c->card; i++) out[c->arr[i] >> 6] |= (uint64_t)1 << (c->arr[i] & 63);
}
static Cont *from_words(const uint64_t *w) {
    Cont *c = cont_new(); int card = 0;
    for (int i = 0; i < 1024; i++) card += pop64(w[i]);
    if (card == 0) { free(c); return NULL; }
    c->bits = malloc(1024 * sizeof(uint64_t)); memcpy(c->bits, w, 1024 * sizeof(uint64_t)); c->is_bitmap = 1; c->card = card;
    if (card <= ARRAY_MAX) to_array(c);
    return c;
}
static Cont *cont_op(const Cont *a, const Cont *b, char op) {
    uint64_t wa[1024], wb[1024], wr[1024];
    as_words(a, wa); as_words(b, wb);
    for (int i = 0; i < 1024; i++) wr[i] = op == '&' ? wa[i] & wb[i] : op == '|' ? wa[i] | wb[i] : wa[i] & ~wb[i];
    return from_words(wr);
}
static Cont *cont_copy(const Cont *a) { uint64_t w[1024]; as_words(a, w); return from_words(w); }

static void r_add(Roar *r, uint32_t v) { int k = (int)(v >> 16); if (!r->c[k]) r->c[k] = cont_new(); cont_add(r->c[k], (uint16_t)v); }
static void r_del(Roar *r, uint32_t v) { int k = (int)(v >> 16); if (!r->c[k]) return; cont_del(r->c[k], (uint16_t)v); if (r->c[k]->card == 0) { cont_free(r->c[k]); r->c[k] = NULL; } }
static int r_has(const Roar *r, uint32_t v) { int k = (int)(v >> 16); return r->c[k] && cont_has(r->c[k], (uint16_t)v); }
static long r_card(const Roar *r) { long n = 0; for (int k = 0; k < CHUNKS; k++) if (r->c[k]) n += r->c[k]->card; return n; }
static Roar r_op(const Roar *a, const Roar *b, char op) {
    Roar o; memset(&o, 0, sizeof o);
    for (int k = 0; k < CHUNKS; k++) {
        if (a->c[k] && b->c[k]) o.c[k] = cont_op(a->c[k], b->c[k], op);
        else if (a->c[k] && op != '&') o.c[k] = cont_copy(a->c[k]);
        else if (b->c[k] && op == '|') o.c[k] = cont_copy(b->c[k]);
    }
    return o;
}
static void r_free(Roar *r) { for (int k = 0; k < CHUNKS; k++) { cont_free(r->c[k]); r->c[k] = NULL; } }
static void r_stats(const Roar *r, int *arrs, int *bms, long *bytes) {
    *arrs = *bms = 0; *bytes = 0;
    for (int k = 0; k < CHUNKS; k++) if (r->c[k]) { if (r->c[k]->is_bitmap) { (*bms)++; *bytes += 8192; } else { (*arrs)++; *bytes += 2L * r->c[k]->card; } }
}
static void verify(const Roar *r, const unsigned char *ref, const char *what) {
    long n = 0;
    for (uint32_t v = 0; v < UNIVERSE; v++) { check(r_has(r, v) == ref[v], what); n += ref[v]; }
    check(r_card(r) == n, "cardinality");
    for (int k = 0; k < CHUNKS; k++) if (r->c[k]) check(r->c[k]->is_bitmap ? r->c[k]->card > ARRAY_MAX : r->c[k]->card <= ARRAY_MAX, "container kind matches cardinality");
}

int main(void) {
    static unsigned char ra[UNIVERSE], rb[UNIVERSE], rr[UNIVERSE];
    Roar a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    /* A: mixture of dense chunks, sparse chunks and empty chunks. */
    int dens[CHUNKS] = {0, 90, 2, 0, 50, 7, 100, 0, 1, 70, 0, 3, 40, 0, 6, 80};
    for (int k = 0; k < CHUNKS; k++) for (uint32_t i = 0; i < 65536; i++) {
        uint32_t v = (uint32_t)k * 65536u + i; unsigned x = rnd() % 100;
        if ((int)x < dens[k]) { r_add(&a, v); ra[v] = 1; }
        unsigned y = rnd() % 100;
        if ((int)y < dens[(k + 3) % CHUNKS]) { r_add(&b, v); rb[v] = 1; }
    }
    verify(&a, ra, "A contents"); verify(&b, rb, "B contents");
    int na, nb; long by;
    r_stats(&a, &na, &nb, &by);
    printf("A: card=%ld array_containers=%d bitmap_containers=%d bytes=%ld (flat bitmap %u)\n", r_card(&a), na, nb, by, UNIVERSE / 8);
    r_stats(&b, &na, &nb, &by);
    printf("B: card=%ld array_containers=%d bitmap_containers=%d bytes=%ld\n", r_card(&b), na, nb, by);
    const char ops[3] = {'&', '|', '-'};
    for (int o = 0; o < 3; o++) {
        Roar r = r_op(&a, &b, ops[o] == '-' ? 'n' : ops[o]);
        for (uint32_t v = 0; v < UNIVERSE; v++) rr[v] = ops[o] == '&' ? ra[v] & rb[v] : ops[o] == '|' ? ra[v] | rb[v] : ra[v] & !rb[v];
        verify(&r, rr, "set operation");
        r_stats(&r, &na, &nb, &by);
        printf("A %c B: card=%ld arrays=%d bitmaps=%d\n", ops[o], r_card(&r), na, nb);
        r_free(&r);
    }
    /* mutations crossing the 4096 threshold in both directions */
    int crossings_up = 0, crossings_down = 0;
    for (int step = 0; step < 40000; step++) {
        uint32_t chunk = 2u + rnd() % 2u;
        uint32_t low = rnd() % 7000u;
        uint32_t v = chunk * 65536u + low;
        int was = a.c[v >> 16] ? a.c[v >> 16]->is_bitmap : 0;
        if (rnd() % 100 < 60) { r_add(&a, v); ra[v] = 1; } else { r_del(&a, v); ra[v] = 0; }
        int now = a.c[v >> 16] ? a.c[v >> 16]->is_bitmap : 0;
        crossings_up += !was && now; crossings_down += was && !now;
    }
    verify(&a, ra, "after mutation");
    r_stats(&a, &na, &nb, &by);
    printf("after mutations: card=%ld arrays=%d bitmaps=%d up=%d down=%d\n", r_card(&a), na, nb, crossings_up, crossings_down);
    r_free(&a); r_free(&b);
    return 0;
}
