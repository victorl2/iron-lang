/*
 * title: Inverted index with varint delta postings, skip intersection and phrase queries
 * topic: data_structures
 * covers: postings lists, delta and varint encoding, positional index, skip pointers, boolean and phrase queries, brute-force scan oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 2718281u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define NDOCS 250
#define VOCAB 40
#define MAXTOK 50

static int doc_tok[NDOCS][MAXTOK];
static int doc_len[NDOCS];

typedef struct { int doc, tf, pstart; } Post;
typedef struct {
    int n;
    Post *p;
    int *pos;
    int npos;
    unsigned char *enc;
    size_t enclen;
} Plist;

static Plist idx[VOCAB];

/* varint: 7 bits per byte, high bit means more follow */
static size_t put_varint(unsigned char *out, unsigned v) {
    size_t n = 0;
    while (v >= 128) { out[n++] = (unsigned char)(v | 128); v >>= 7; }
    out[n++] = (unsigned char)v;
    return n;
}
static unsigned get_varint(const unsigned char *in, size_t *at) {
    unsigned v = 0, shift = 0;
    for (;;) {
        unsigned char b = in[(*at)++];
        v |= (unsigned)(b & 127) << shift;
        if (!(b & 128)) return v;
        shift += 7;
    }
}

static void encode(Plist *pl) {
    pl->enc = malloc((size_t)(pl->n * 3 + pl->npos) * 5 + 8);
    CHECK(pl->enc);
    size_t at = 0;
    at += put_varint(pl->enc + at, (unsigned)pl->n);
    int prev_doc = 0;
    for (int i = 0; i < pl->n; i++) {
        at += put_varint(pl->enc + at, (unsigned)(pl->p[i].doc - prev_doc));
        prev_doc = pl->p[i].doc;
        at += put_varint(pl->enc + at, (unsigned)pl->p[i].tf);
        int prev_pos = 0;
        for (int k = 0; k < pl->p[i].tf; k++) {
            int pos = pl->pos[pl->p[i].pstart + k];
            at += put_varint(pl->enc + at, (unsigned)(pos - prev_pos));
            prev_pos = pos;
        }
    }
    pl->enclen = at;
}
static void decode(const Plist *src, Plist *dst) {
    size_t at = 0;
    dst->n = (int)get_varint(src->enc, &at);
    dst->p = malloc((size_t)(dst->n + 1) * sizeof(Post));
    dst->pos = malloc((size_t)(src->npos + 1) * sizeof(int));
    CHECK(dst->p && dst->pos);
    int doc = 0, np = 0;
    for (int i = 0; i < dst->n; i++) {
        doc += (int)get_varint(src->enc, &at);
        dst->p[i].doc = doc;
        dst->p[i].tf = (int)get_varint(src->enc, &at);
        dst->p[i].pstart = np;
        int pos = 0;
        for (int k = 0; k < dst->p[i].tf; k++) { pos += (int)get_varint(src->enc, &at); dst->pos[np++] = pos; }
    }
    dst->npos = np;
    dst->enc = NULL; dst->enclen = 0;
    CHECK(at == src->enclen);
}

/* intersection of two doc lists with skip pointers of stride sqrt(n); counts comparisons */
static long cmp_skip, cmp_naive;
static int intersect_skip(const Plist *a, const Plist *b, int *out) {
    int sa = 1, sb = 1;
    while (sa * sa < a->n) sa++;
    while (sb * sb < b->n) sb++;
    int i = 0, j = 0, n = 0;
    while (i < a->n && j < b->n) {
        cmp_skip++;
        if (a->p[i].doc == b->p[j].doc) { out[n++] = a->p[i].doc; i++; j++; }
        else if (a->p[i].doc < b->p[j].doc) {
            if (i % sa == 0 && i + sa < a->n && a->p[i + sa].doc <= b->p[j].doc) i += sa; else i++;
        } else {
            if (j % sb == 0 && j + sb < b->n && b->p[j + sb].doc <= a->p[i].doc) j += sb; else j++;
        }
    }
    return n;
}
static int intersect_naive(const Plist *a, const Plist *b, int *out) {
    int i = 0, j = 0, n = 0;
    while (i < a->n && j < b->n) {
        cmp_naive++;
        if (a->p[i].doc == b->p[j].doc) { out[n++] = a->p[i].doc; i++; j++; }
        else if (a->p[i].doc < b->p[j].doc) i++; else j++;
    }
    return n;
}
static int union_docs(const Plist *a, const Plist *b, int *out) {
    int i = 0, j = 0, n = 0;
    while (i < a->n || j < b->n) {
        if (j >= b->n || (i < a->n && a->p[i].doc < b->p[j].doc)) out[n++] = a->p[i++].doc;
        else if (i >= a->n || b->p[j].doc < a->p[i].doc) out[n++] = b->p[j++].doc;
        else { out[n++] = a->p[i].doc; i++; j++; }
    }
    return n;
}
static int andnot_docs(const Plist *a, const Plist *b, int *out) {
    int i = 0, j = 0, n = 0;
    while (i < a->n) {
        while (j < b->n && b->p[j].doc < a->p[i].doc) j++;
        if (j < b->n && b->p[j].doc == a->p[i].doc) { i++; continue; }
        out[n++] = a->p[i++].doc;
    }
    return n;
}
/* phrase "a b": docs where some position p of a has p+1 in b */
static int phrase_docs(const Plist *a, const Plist *b, int *out) {
    int i = 0, j = 0, n = 0;
    while (i < a->n && j < b->n) {
        if (a->p[i].doc < b->p[j].doc) i++;
        else if (a->p[i].doc > b->p[j].doc) j++;
        else {
            int hit = 0;
            for (int x = 0; x < a->p[i].tf && !hit; x++)
                for (int y = 0; y < b->p[j].tf; y++)
                    if (b->pos[b->p[j].pstart + y] == a->pos[a->p[i].pstart + x] + 1) { hit = 1; break; }
            if (hit) out[n++] = a->p[i].doc;
            i++; j++;
        }
    }
    return n;
}
static int has_word(int d, int w) { for (int t = 0; t < doc_len[d]; t++) if (doc_tok[d][t] == w) return 1; return 0; }
static int has_phrase(int d, int a, int b) { for (int t = 0; t + 1 < doc_len[d]; t++) if (doc_tok[d][t] == a && doc_tok[d][t + 1] == b) return 1; return 0; }

int main(void) {
    for (int d = 0; d < NDOCS; d++) {
        doc_len[d] = 5 + (int)(rnd() % (MAXTOK - 5));
        for (int t = 0; t < doc_len[d]; t++) {
            int a = (int)(rnd() % VOCAB), b = (int)(rnd() % VOCAB);
            doc_tok[d][t] = a % 3 == 0 ? b : a * b / VOCAB;   /* mostly skewed toward small ids */
        }
    }
    /* raw build */
    for (int w = 0; w < VOCAB; w++) {
        Plist *pl = &idx[w];
        pl->p = malloc(NDOCS * sizeof(Post));
        pl->pos = malloc((size_t)NDOCS * MAXTOK * sizeof(int));
        CHECK(pl->p && pl->pos);
        for (int d = 0; d < NDOCS; d++) {
            int tf = 0, start = pl->npos;
            for (int t = 0; t < doc_len[d]; t++) if (doc_tok[d][t] == w) { pl->pos[pl->npos++] = t; tf++; }
            if (tf) { pl->p[pl->n].doc = d; pl->p[pl->n].tf = tf; pl->p[pl->n].pstart = start; pl->n++; }
        }
        encode(pl);
    }
    /* decode and compare roundtrip, then work on the decoded lists */
    static Plist dec[VOCAB];
    size_t raw_bytes = 0, enc_bytes = 0;
    long postings = 0;
    for (int w = 0; w < VOCAB; w++) {
        decode(&idx[w], &dec[w]);
        CHECK(dec[w].n == idx[w].n && dec[w].npos == idx[w].npos);
        for (int i = 0; i < idx[w].n; i++) CHECK(dec[w].p[i].doc == idx[w].p[i].doc && dec[w].p[i].tf == idx[w].p[i].tf && dec[w].p[i].pstart == idx[w].p[i].pstart);
        CHECK(memcmp(dec[w].pos, idx[w].pos, (size_t)idx[w].npos * sizeof(int)) == 0);
        raw_bytes += (size_t)idx[w].n * 2 * sizeof(int) + (size_t)idx[w].npos * sizeof(int);
        enc_bytes += idx[w].enclen;
        postings += idx[w].n;
    }
    printf("index: %d docs, %d words, %ld postings, raw %zu bytes -> varint %zu bytes (%.1f%%)\n", NDOCS, VOCAB, postings, raw_bytes, enc_bytes, 100.0 * (double)enc_bytes / (double)raw_bytes);
    long q_and = 0, q_or = 0, q_not = 0, q_phrase = 0, hits = 0;
    static int out[NDOCS], out2[NDOCS];
    for (int q = 0; q < 300; q++) {
        int a = (int)(rnd() % VOCAB), b = (int)(rnd() % VOCAB);
        int n1 = intersect_skip(&dec[a], &dec[b], out);
        int n2 = intersect_naive(&dec[a], &dec[b], out2);
        CHECK(n1 == n2 && memcmp(out, out2, (size_t)n1 * sizeof(int)) == 0);
        int expect = 0;
        for (int d = 0; d < NDOCS; d++) if (has_word(d, a) && has_word(d, b)) { CHECK(expect < n1 && out[expect] == d); expect++; }
        CHECK(expect == n1);
        q_and++; hits += n1;
        int nu = union_docs(&dec[a], &dec[b], out);
        expect = 0;
        for (int d = 0; d < NDOCS; d++) if (has_word(d, a) || has_word(d, b)) { CHECK(expect < nu && out[expect] == d); expect++; }
        CHECK(expect == nu);
        q_or++; hits += nu;
        int nn = andnot_docs(&dec[a], &dec[b], out);
        expect = 0;
        for (int d = 0; d < NDOCS; d++) if (has_word(d, a) && !has_word(d, b)) { CHECK(expect < nn && out[expect] == d); expect++; }
        CHECK(expect == nn);
        q_not++; hits += nn;
        int np = phrase_docs(&dec[a], &dec[b], out);
        expect = 0;
        for (int d = 0; d < NDOCS; d++) if (has_phrase(d, a, b)) { CHECK(expect < np && out[expect] == d); expect++; }
        CHECK(expect == np);
        q_phrase++; hits += np;
    }
    printf("queries: and=%ld or=%ld andnot=%ld phrase=%ld, total hits=%ld\n", q_and, q_or, q_not, q_phrase, hits);
    printf("skip intersection comparisons=%ld, plain merge comparisons=%ld\n", cmp_skip, cmp_naive);
    CHECK(cmp_skip <= cmp_naive);
    int top = 0;
    for (int w = 1; w < VOCAB; w++) if (idx[w].n > idx[top].n) top = w;
    int rare = -1;
    for (int w = 0; w < VOCAB; w++) if (idx[w].n > 0 && (rare < 0 || idx[w].n < idx[rare].n)) rare = w;
    printf("most common word %d is in %d of %d docs; rarest present word %d is in %d\n", top, idx[top].n, NDOCS, rare, idx[rare].n);
    for (int w = 0; w < VOCAB; w++) { free(idx[w].p); free(idx[w].pos); free(idx[w].enc); free(dec[w].p); free(dec[w].pos); }
    return 0;
}
