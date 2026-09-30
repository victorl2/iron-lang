/*
 * title: Bencode encoder and strict canonical decoder
 * topic: io_files
 * covers: bencode integers, byte strings, lists, dictionaries, sorted keys, leading zero and negative zero rejection, depth limit, torrent-like metainfo
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
void bput(Buf *b, const void *s, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + k) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) fail("oom");
        b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, s, k);
    b->n += k;
}
void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

enum { B_INT, B_STR, B_LIST, B_DICT };

typedef struct B {
    int t;
    long long i;
    unsigned char *s;
    size_t slen;
    struct B **kid;
    unsigned char **key;
    size_t *klen;
    int n;
} B;

static B *bnew(int t) { B *b = calloc(1, sizeof *b); if (!b) fail("oom"); b->t = t; return b; }
static B *bint(long long v) { B *b = bnew(B_INT); b->i = v; return b; }
static B *bstrn(const void *s, size_t n) { B *b = bnew(B_STR); b->s = malloc(n + 1); memcpy(b->s, s, n); b->s[n] = 0; b->slen = n; return b; }
static B *bcs(const char *s) { return bstrn(s, strlen(s)); }
static void bfree_node(B *b) {
    for (int i = 0; i < b->n; i++) { bfree_node(b->kid[i]); if (b->key) free(b->key[i]); }
    free(b->kid); free(b->key); free(b->klen); free(b->s); free(b);
}
static void push(B *c, const void *k, size_t kl, B *v) {
    c->kid = realloc(c->kid, (size_t)(c->n + 1) * sizeof *c->kid);
    if (c->t == B_DICT) {
        c->key = realloc(c->key, (size_t)(c->n + 1) * sizeof *c->key);
        c->klen = realloc(c->klen, (size_t)(c->n + 1) * sizeof *c->klen);
        c->key[c->n] = malloc(kl + 1);
        memcpy(c->key[c->n], k, kl);
        c->klen[c->n] = kl;
    }
    c->kid[c->n++] = v;
}

static int keycmp(const B *d, int a, int b) {
    size_t la = d->klen[a], lb = d->klen[b];
    int c = memcmp(d->key[a], d->key[b], la < lb ? la : lb);
    return c ? c : (la < lb ? -1 : la > lb);
}

static void encode(Buf *o, const B *b) {
    char t[32];
    switch (b->t) {
    case B_INT: snprintf(t, sizeof t, "i%llde", b->i); bstr(o, t); break;
    case B_STR: snprintf(t, sizeof t, "%zu:", b->slen); bstr(o, t); bput(o, b->s, b->slen); break;
    case B_LIST: bbyte(o, 'l'); for (int i = 0; i < b->n; i++) encode(o, b->kid[i]); bbyte(o, 'e'); break;
    default: {
        int idx[16];
        for (int i = 0; i < b->n; i++) idx[i] = i;
        for (int i = 1; i < b->n; i++) {           /* insertion sort by raw key bytes */
            int x = idx[i], j = i - 1;
            while (j >= 0 && keycmp(b, idx[j], x) > 0) { idx[j + 1] = idx[j]; j--; }
            idx[j + 1] = x;
        }
        bbyte(o, 'd');
        for (int i = 0; i < b->n; i++) {
            snprintf(t, sizeof t, "%zu:", b->klen[idx[i]]);
            bstr(o, t);
            bput(o, b->key[idx[i]], b->klen[idx[i]]);
            encode(o, b->kid[idx[i]]);
        }
        bbyte(o, 'e');
    }
    }
}

typedef struct { const unsigned char *p; size_t n, pos; const char *err; } D;

static int bytes_cmp(const unsigned char *a, size_t la, const unsigned char *b, size_t lb) {
    int c = memcmp(a, b, la < lb ? la : lb);
    return c ? c : (la < lb ? -1 : la > lb);
}

static B *decode(D *d, int depth) {
    if (depth > 8) { d->err = "nesting too deep"; return NULL; }
    if (d->pos >= d->n) { d->err = "unexpected end"; return NULL; }
    unsigned char c = d->p[d->pos];
    if (c == 'i') {
        size_t q = d->pos + 1;
        int neg = 0;
        if (q < d->n && d->p[q] == '-') { neg = 1; q++; }
        size_t ds = q;
        long long v = 0;
        while (q < d->n && d->p[q] >= '0' && d->p[q] <= '9') {
            if (v > 900000000000000000LL) { d->err = "integer overflow"; return NULL; }
            v = v * 10 + (d->p[q] - '0');
            q++;
        }
        if (q == ds || q >= d->n || d->p[q] != 'e') { d->err = "malformed integer"; return NULL; }
        if (d->p[ds] == '0' && q - ds > 1) { d->err = "leading zero"; return NULL; }
        if (neg && v == 0) { d->err = "negative zero"; return NULL; }
        d->pos = q + 1;
        return bint(neg ? -v : v);
    }
    if (c >= '0' && c <= '9') {
        size_t q = d->pos, len = 0;
        if (c == '0' && q + 1 < d->n && d->p[q + 1] >= '0' && d->p[q + 1] <= '9') { d->err = "leading zero in length"; return NULL; }
        while (q < d->n && d->p[q] >= '0' && d->p[q] <= '9') { len = len * 10 + (d->p[q] - '0'); q++; if (len > 1000000) { d->err = "length too large"; return NULL; } }
        if (q >= d->n || d->p[q] != ':') { d->err = "missing colon"; return NULL; }
        q++;
        if (len > d->n - q) { d->err = "string overruns input"; return NULL; }
        B *b = bstrn(d->p + q, len);
        d->pos = q + len;
        return b;
    }
    if (c == 'l' || c == 'd') {
        B *b = bnew(c == 'l' ? B_LIST : B_DICT);
        d->pos++;
        for (;;) {
            if (d->pos >= d->n) { d->err = "unterminated container"; bfree_node(b); return NULL; }
            if (d->p[d->pos] == 'e') { d->pos++; return b; }
            if (c == 'd') {
                B *k = decode(d, depth + 1);
                if (!k) { bfree_node(b); return NULL; }
                if (k->t != B_STR) { d->err = "dictionary key is not a string"; bfree_node(k); bfree_node(b); return NULL; }
                if (b->n && bytes_cmp(b->key[b->n - 1], b->klen[b->n - 1], k->s, k->slen) >= 0) {
                    d->err = "dictionary keys not strictly sorted"; bfree_node(k); bfree_node(b); return NULL;
                }
                B *v = decode(d, depth + 1);
                if (!v) { bfree_node(k); bfree_node(b); return NULL; }
                push(b, k->s, k->slen, v);
                bfree_node(k);
            } else {
                B *v = decode(d, depth + 1);
                if (!v) { bfree_node(b); return NULL; }
                push(b, NULL, 0, v);
            }
        }
    }
    d->err = "unexpected byte";
    return NULL;
}

static B *gen(int depth) {
    uint32_t k = rnd() % (depth > 0 ? 5u : 2u);
    if (k == 0) {
        uint32_t r1 = rnd(), r2 = rnd();
        return bint((long long)(int32_t)r1 * (r2 & 1 ? 1000 : 1));
    }
    if (k == 1) {
        unsigned char s[12];
        size_t l = rnd() % 10;
        for (size_t i = 0; i < l; i++) s[i] = (unsigned char)(rnd() >> 11);
        return bstrn(s, l);
    }
    if (k == 2 || k == 3) {
        B *l = bnew(B_LIST);
        uint32_t n = rnd() % 4;
        for (uint32_t i = 0; i < n; i++) push(l, NULL, 0, gen(depth - 1));
        return l;
    }
    B *d = bnew(B_DICT);
    uint32_t n = 1 + rnd() % 4;
    for (uint32_t i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof key, "k%u%c", (unsigned)(rnd() % 100), (char)('a' + i));
        push(d, key, strlen(key), gen(depth - 1));
    }
    return d;
}

static void show(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] >= 32 && p[i] < 127) putchar(p[i]); else printf("\\x%02x", p[i]);
    }
}

int main(void) {
    /* torrent-like metainfo, keys inserted out of order on purpose */
    B *info = bnew(B_DICT);
    push(info, "pieces", 6, bstrn("\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14", 20));
    push(info, "name", 4, bcs("sample.txt"));
    push(info, "length", 6, bint(1048576));
    push(info, "piece length", 12, bint(262144));
    B *meta = bnew(B_DICT);
    push(meta, "info", 4, info);
    B *tiers = bnew(B_LIST);
    push(tiers, NULL, 0, bcs("http://tracker.example/announce"));
    push(meta, "announce-list", 13, tiers);
    push(meta, "creation date", 13, bint(1700000000));
    Buf enc = {0};
    encode(&enc, meta);
    wfile("meta.torrent", enc.p, enc.n);
    size_t n;
    unsigned char *d = rfile("meta.torrent", &n);
    printf("metainfo %zu bytes: ", n);
    show(d, n);
    putchar('\n');
    D dec = {d, n, 0, NULL};
    B *back = decode(&dec, 0);
    CHECK(back && dec.pos == n);
    Buf again = {0};
    encode(&again, back);
    CHECK(again.n == n && !memcmp(again.p, d, n));
    printf("re-encode identical; top-level keys sorted:");
    for (int i = 0; i < back->n; i++) printf(" %.*s", (int)back->klen[i], back->key[i]);
    putchar('\n');
    bfree_node(back); bfree_node(meta); bfree(&again); bfree(&enc); free(d);

    int total = 0;
    for (int i = 0; i < 60; i++) {
        B *g = gen(4);
        Buf e = {0}, e2 = {0};
        encode(&e, g);
        D dd = {e.p, e.n, 0, NULL};
        B *r = decode(&dd, 0);
        if (!r) { fprintf(stderr, "decode failed: %s\n", dd.err); exit(1); }
        CHECK(dd.pos == e.n);
        encode(&e2, r);
        CHECK(e.n == e2.n && !memcmp(e.p, e2.p, e.n));
        total += (int)e.n;
        if (i < 3) { printf("random %d: ", i); show(e.p, e.n < 60 ? e.n : 60); printf("%s\n", e.n > 60 ? "..." : ""); }
        bfree_node(g); bfree_node(r); bfree(&e); bfree(&e2);
    }
    printf("60 random trees round-tripped, %d encoded bytes\n", total);
    static const char *bad[] = {
        "i03e", "i-0e", "i12", "ie", "5:abc", "03:abc", "l", "d3:foo3:bar3:bar3:baze", "d3:zzzi1e3:aaai2ee", "di1ei2ee",
        "x", "i9223372036854775808e", "lllllllllllllllllllee", "d3:fooi1e3:foo1:xe"
    };
    for (int i = 0; i < 14; i++) {
        D dd = {(const unsigned char *)bad[i], strlen(bad[i]), 0, NULL};
        B *r = decode(&dd, 0);
        CHECK(r == NULL);
        printf("bad %2d %-26s %s\n", i, bad[i], dd.err);
    }
    remove("meta.torrent");
    return 0;
}
