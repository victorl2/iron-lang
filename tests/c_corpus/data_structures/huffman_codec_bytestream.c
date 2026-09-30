/*
 * title: Huffman tree codec over a byte stream
 * topic: data_structures
 * covers: huffman tree, bit packing, tree serialization, decoding by tree walk, two-queue cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct HNode {
    unsigned long long w;
    int sym;   /* -1 for internal */
    int order; /* creation order for deterministic tie breaks */
    struct HNode *l, *r;
} HNode;

static uint64_t rs = 0x2545F4914F6CDD1DULL;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}

/* binary min-heap of HNode* ordered by (w, order) */
typedef struct { HNode *a[600]; int n; } Heap;
static int less(const HNode *x, const HNode *y) {
    return x->w != y->w ? x->w < y->w : x->order < y->order;
}
static void hpush(Heap *h, HNode *x) {
    int i = h->n++;
    while (i > 0 && less(x, h->a[(i - 1) / 2])) { h->a[i] = h->a[(i - 1) / 2]; i = (i - 1) / 2; }
    h->a[i] = x;
}
static HNode *hpop(Heap *h) {
    HNode *top = h->a[0], *x = h->a[--h->n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n) break;
        if (c + 1 < h->n && less(h->a[c + 1], h->a[c])) c++;
        if (!less(h->a[c], x)) break;
        h->a[i] = h->a[c]; i = c;
    }
    if (h->n) h->a[i] = x;
    return top;
}
static void destroy(HNode *n) { if (n) { destroy(n->l); destroy(n->r); free(n); } }

static HNode *build(const unsigned long long *freq, unsigned long long *cost) {
    Heap h; h.n = 0;
    int order = 0;
    for (int s = 0; s < 256; s++)
        if (freq[s]) {
            HNode *n = calloc(1, sizeof *n);
            n->w = freq[s]; n->sym = s; n->order = order++;
            hpush(&h, n);
        }
    *cost = 0;
    if (h.n == 0) return NULL;
    while (h.n > 1) {
        HNode *a = hpop(&h), *b = hpop(&h);
        HNode *p = calloc(1, sizeof *p);
        p->w = a->w + b->w; p->sym = -1; p->order = order++; p->l = a; p->r = b;
        *cost += p->w;
        hpush(&h, p);
    }
    return hpop(&h);
}

/* independent cost: two-queue method on sorted weights */
static unsigned long long two_queue_cost(const unsigned long long *freq) {
    unsigned long long q1[256], q2[256]; int n1 = 0, h1 = 0, n2 = 0, h2 = 0;
    for (int s = 0; s < 256; s++) if (freq[s]) q1[n1++] = freq[s];
    for (int i = 1; i < n1; i++) { unsigned long long x = q1[i]; int j = i - 1; while (j >= 0 && q1[j] > x) { q1[j + 1] = q1[j]; j--; } q1[j + 1] = x; }
    unsigned long long cost = 0;
    int left = n1;
    while (left + (n2 - h2) > 1) {
        unsigned long long t[2];
        for (int k = 0; k < 2; k++) {
            if (h1 < n1 && (h2 >= n2 || q1[h1] <= q2[h2])) { t[k] = q1[h1++]; }
            else { t[k] = q2[h2++]; }
        }
        left = n1 - h1;
        q2[n2++] = t[0] + t[1]; cost += t[0] + t[1];
    }
    return cost;
}

typedef struct { unsigned char buf[8192]; size_t nbits; } Bits;
static void put(Bits *b, int bit) {
    if (bit) b->buf[b->nbits >> 3] |= (unsigned char)(0x80u >> (b->nbits & 7));
    b->nbits++;
}
static int get(const Bits *b, size_t *pos) {
    int bit = (b->buf[*pos >> 3] >> (7 - (*pos & 7))) & 1;
    (*pos)++;
    return bit;
}

static void codes(const HNode *n, unsigned long long *code, int *len, unsigned long long c, int d) {
    if (n->sym >= 0) { code[n->sym] = c; len[n->sym] = d ? d : 1; return; }
    codes(n->l, code, len, c << 1, d + 1);
    codes(n->r, code, len, (c << 1) | 1, d + 1);
}
/* preorder serialization: 1 + 8 bits for leaf, 0 for internal */
static void ser(const HNode *n, Bits *b) {
    if (n->sym >= 0) { put(b, 1); for (int i = 7; i >= 0; i--) put(b, (n->sym >> i) & 1); return; }
    put(b, 0); ser(n->l, b); ser(n->r, b);
}
static HNode *deser(const Bits *b, size_t *pos) {
    HNode *n = calloc(1, sizeof *n);
    if (get(b, pos)) { int s = 0; for (int i = 0; i < 8; i++) s = (s << 1) | get(b, pos); n->sym = s; return n; }
    n->sym = -1; n->l = deser(b, pos); n->r = deser(b, pos);
    return n;
}

static int roundtrip(const char *name, const unsigned char *data, size_t n) {
    unsigned long long freq[256] = {0};
    for (size_t i = 0; i < n; i++) freq[data[i]]++;
    unsigned long long cost;
    HNode *root = build(freq, &cost);
    int distinct = 0;
    for (int s = 0; s < 256; s++) distinct += freq[s] != 0;
    static Bits enc;
    memset(&enc, 0, sizeof enc);
    unsigned long long code[256] = {0}; int len[256] = {0};
    if (root) {
        codes(root, code, len, 0, 0);
        ser(root, &enc);
    }
    size_t header = enc.nbits;
    for (size_t i = 0; i < n; i++)
        for (int k = len[data[i]] - 1; k >= 0; k--) put(&enc, (int)((code[data[i]] >> k) & 1));
    size_t payload = enc.nbits - header;
    unsigned long long expect_bits = 0;
    for (int s = 0; s < 256; s++) expect_bits += freq[s] * (unsigned)len[s];
    check(payload == expect_bits, "payload bits = sum freq*len");
    if (distinct > 1) check(expect_bits == cost, "cost = sum of internal weights");
    check(cost == two_queue_cost(freq) || distinct <= 1, "two-queue optimal cost");
    /* Kraft equality for a full tree */
    if (distinct > 1) {
        unsigned long long kraft = 0;
        for (int s = 0; s < 256; s++) if (freq[s]) kraft += 1ULL << (40 - len[s]);
        check(kraft == 1ULL << 40, "kraft sum is 1");
    }
    /* decode */
    size_t pos = 0;
    HNode *dt = root ? deser(&enc, &pos) : NULL;
    unsigned char *out = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        const HNode *c = dt;
        if (c->sym < 0) { while (c->sym < 0) c = get(&enc, &pos) ? c->r : c->l; }
        else pos++; /* single-symbol tree: one bit per symbol */
        out[i] = (unsigned char)c->sym;
    }
    check(pos == enc.nbits, "consumed exactly all bits");
    check(memcmp(out, data, n) == 0, "decoded equals input");
    size_t bytes = (enc.nbits + 7) / 8;
    printf("%-10s n=%5zu distinct=%3d header=%4zu bits payload=%6zu bits -> %5zu bytes\n", name, n, distinct, header, payload, bytes);
    if (distinct >= 2) {
        int mn = 99, mx = 0;
        for (int s = 0; s < 256; s++) if (freq[s]) { if (len[s] < mn) mn = len[s]; if (len[s] > mx) mx = len[s]; }
        printf("           code lengths %d..%d, cost %llu\n", mn, mx, cost);
    }
    free(out); destroy(root); destroy(dt);
    return (int)bytes;
}

int main(void) {
    static unsigned char buf[4000];
    const char *txt = "it was the best of times, it was the worst of times, it was the age of wisdom, it was the age of foolishness";
    size_t n = strlen(txt);
    memcpy(buf, txt, n);
    roundtrip("english", buf, n);
    roundtrip("empty", buf, 0);
    memset(buf, 'z', 300);
    roundtrip("single", buf, 300);
    for (int i = 0; i < 2000; i++) { unsigned r = rnd() % 100; buf[i] = (unsigned char)(r < 60 ? 'a' : r < 85 ? 'b' : r < 95 ? 'c' : 'd' + (rnd() % 4)); }
    roundtrip("skewed", buf, 2000);
    for (int i = 0; i < 4000; i++) buf[i] = (unsigned char)(i & 255);
    roundtrip("uniform", buf, 4000);
    for (int i = 0; i < 4000; i++) buf[i] = (unsigned char)(rnd() & 0xff);
    roundtrip("noise", buf, 4000);
    /* fibonacci frequencies give a maximally skewed tree */
    unsigned long long f[256] = {0}; unsigned long long a = 1, b = 1;
    size_t m = 0;
    for (int s = 0; s < 14; s++) { for (unsigned long long k = 0; k < a; k++) buf[m++] = (unsigned char)(s + 65); unsigned long long t = a + b; a = b; b = t; }
    (void)f;
    roundtrip("fibonacci", buf, m);
    return 0;
}
