/*
 * title: Protobuf wire format writer and schema-less dumper
 * topic: networking
 * covers: varint and zigzag, fixed32 and fixed64, length-delimited fields, packed repeats, groups, overlong varint rejection, non-canonical detection, nested message heuristic, string versus bytes guess
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { unsigned char b[2048]; size_t n; } W;

static void wvar(W *w, uint64_t v) {
    while (v >= 128) { w->b[w->n++] = (unsigned char)(v | 128); v >>= 7; }
    w->b[w->n++] = (unsigned char)v;
}
static uint64_t zz64(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63); }
static int64_t unzz64(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }
static void wkey(W *w, unsigned num, unsigned wt) { wvar(w, ((uint64_t)num << 3) | wt); }
static void f_varint(W *w, unsigned num, uint64_t v) { wkey(w, num, 0); wvar(w, v); }
static void f_sint(W *w, unsigned num, int64_t v) { f_varint(w, num, zz64(v)); }
static void f_fixed32(W *w, unsigned num, uint32_t v) { wkey(w, num, 5); for (int i = 0; i < 4; i++) w->b[w->n++] = (unsigned char)(v >> (8 * i)); }
static void f_fixed64(W *w, unsigned num, uint64_t v) { wkey(w, num, 1); for (int i = 0; i < 8; i++) w->b[w->n++] = (unsigned char)(v >> (8 * i)); }
static void f_bytes(W *w, unsigned num, const void *d, size_t n) { wkey(w, num, 2); wvar(w, n); memcpy(w->b + w->n, d, n); w->n += n; }
static void f_str(W *w, unsigned num, const char *s) { f_bytes(w, num, s, strlen(s)); }
static void f_msg(W *w, unsigned num, const W *m) { f_bytes(w, num, m->b, m->n); }

/* Varint read: 1 ok, 0 truncated, -1 overflow (more than 10 bytes or 10th byte > 1) */
static int rvar(const unsigned char **p, const unsigned char *end, uint64_t *v, int *canonical) {
    uint64_t r = 0;
    int i;
    for (i = 0; i < 10; i++) {
        if (*p + i >= end) return 0;
        unsigned b = (*p)[i];
        if (i == 9 && b > 1) return -1;
        r |= (uint64_t)(b & 127) << (7 * i);
        if (!(b & 128)) {
            *canonical = !(i > 0 && b == 0);
            *p += i + 1;
            *v = r;
            return 1;
        }
    }
    return -1;
}

typedef struct { unsigned num, wt; uint64_t v; const unsigned char *d; size_t len; int canon; } Field;

static const char *next_field(const unsigned char **p, const unsigned char *end, Field *f) {
    int c1, c2 = 1;
    uint64_t key;
    int r = rvar(p, end, &key, &c1);
    if (r == 0) return "truncated-key";
    if (r < 0) return "bad-varint";
    f->num = (unsigned)(key >> 3);
    f->wt = (unsigned)(key & 7);
    f->canon = c1;
    if (f->num == 0) return "field-number-zero";
    switch (f->wt) {
    case 0:
        r = rvar(p, end, &f->v, &c2);
        if (r == 0) return "truncated-varint";
        if (r < 0) return "bad-varint";
        f->canon = c1 && c2;
        break;
    case 1:
        if (end - *p < 8) return "truncated-fixed64";
        f->v = 0;
        for (int i = 7; i >= 0; i--) f->v = (f->v << 8) | (*p)[i];
        *p += 8;
        break;
    case 2: {
        uint64_t l;
        r = rvar(p, end, &l, &c2);
        if (r == 0) return "truncated-length";
        if (r < 0) return "bad-varint";
        if (l > (uint64_t)(end - *p)) return "length-past-end";
        f->d = *p; f->len = (size_t)l; *p += l;
        break;
    }
    case 3: case 4: f->v = 0; break;
    case 5:
        if (end - *p < 4) return "truncated-fixed32";
        f->v = 0;
        for (int i = 3; i >= 0; i--) f->v = (f->v << 8) | (*p)[i];
        *p += 4;
        break;
    default: return "bad-wire-type";
    }
    return NULL;
}

/* Does the buffer parse cleanly as a message? (used as the nested-message heuristic) */
static int parses(const unsigned char *p, size_t n, int depth) {
    const unsigned char *end = p + n;
    if (n == 0 || depth > 8) return 0;
    int open = 0;
    while (p < end) {
        Field f;
        if (next_field(&p, end, &f)) return 0;
        if (f.wt == 3) open++;
        if (f.wt == 4 && --open < 0) return 0;
    }
    return open == 0;
}

static int printable(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] < 32 || p[i] > 126) return 0;
    return n > 0;
}

static const char *dump(const unsigned char *p, size_t n, int depth, int *fields) {
    const unsigned char *end = p + n;
    char ind[40];
    snprintf(ind, sizeof ind, "%*s", depth * 2, "");
    while (p < end) {
        Field f;
        const char *e = next_field(&p, end, &f);
        if (e) return e;
        (*fields)++;
        switch (f.wt) {
        case 0: {
            int64_t sv = unzz64(f.v);
            printf("%s%u: varint %llu", ind, f.num, (unsigned long long)f.v);
            if ((int64_t)f.v < 0) printf(" (as int64 %lld)", (long long)(int64_t)f.v);
            if (f.v > 1) printf(" (zigzag %lld)", (long long)sv);
            printf("%s\n", f.canon ? "" : " [non-canonical]");
            break;
        }
        case 1: printf("%s%u: fixed64 0x%016llx\n", ind, f.num, (unsigned long long)f.v); break;
        case 5: printf("%s%u: fixed32 0x%08x\n", ind, f.num, (unsigned)f.v); break;
        case 2:
            if (f.len == 0) printf("%s%u: empty\n", ind, f.num);
            else if (printable(f.d, f.len) && !(f.len >= 2 && parses(f.d, f.len, depth + 1) && f.d[0] < 0x20)) printf("%s%u: string \"%.*s\"\n", ind, f.num, (int)f.len, (const char *)f.d);
            else if (parses(f.d, f.len, depth + 1)) {
                printf("%s%u: message {\n", ind, f.num);
                e = dump(f.d, f.len, depth + 1, fields);
                if (e) return e;
                printf("%s}\n", ind);
            } else {
                printf("%s%u: bytes(%zu)", ind, f.num, f.len);
                for (size_t i = 0; i < f.len && i < 8; i++) printf(" %02x", f.d[i]);
                printf("%s\n", f.len > 8 ? " ..." : "");
            }
            break;
        case 3: printf("%s%u: group start\n", ind, f.num); break;
        case 4: printf("%s%u: group end\n", ind, f.num); break;
        default: break;
        }
    }
    return NULL;
}

static uint32_t rs = 0x9b9b9b9bu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    /* Documentation vectors. */
    W w = { { 0 }, 0 };
    f_varint(&w, 1, 150);
    CHECK(w.n == 3 && w.b[0] == 0x08 && w.b[1] == 0x96 && w.b[2] == 0x01);
    W t = { { 0 }, 0 };
    f_str(&t, 2, "testing");
    CHECK(t.n == 9 && t.b[0] == 0x12 && t.b[1] == 0x07);
    W emb = { { 0 }, 0 };
    f_msg(&emb, 3, &w);
    CHECK(emb.n == 5 && memcmp(emb.b, "\x1a\x03\x08\x96\x01", 5) == 0);
    W packed = { { 0 }, 0 };
    { W inner = { { 0 }, 0 }; wvar(&inner, 3); wvar(&inner, 270); wvar(&inner, 86942); f_msg(&packed, 4, &inner); }
    CHECK(packed.n == 8 && memcmp(packed.b, "\x22\x06\x03\x8e\x02\x9e\xa7\x05", 8) == 0);

    /* Big message combining everything. */
    W m = { { 0 }, 0 };
    f_varint(&m, 1, 150);
    f_str(&m, 2, "testing");
    f_msg(&m, 3, &w);
    f_bytes(&m, 4, packed.b + 2, packed.n - 2);
    f_sint(&m, 5, -1);
    f_sint(&m, 5, 2147483647);
    f_varint(&m, 6, (uint64_t)(int64_t)-1);
    f_fixed32(&m, 7, 0x3fc00000u); /* 1.5f */
    f_fixed64(&m, 8, 0x400921fb54442d18ull); /* pi */
    { unsigned char raw[6] = { 0xde, 0xad, 0xbe, 0xef, 0x00, 0xff }; f_bytes(&m, 9, raw, sizeof raw); }
    f_bytes(&m, 10, "", 0);
    { W deep = { { 0 }, 0 }, mid = { { 0 }, 0 }; f_str(&deep, 1, "leaf"); f_varint(&deep, 2, 7); f_msg(&mid, 1, &deep); f_sint(&mid, 3, -300); f_msg(&m, 11, &mid); }
    printf("message is %zu bytes\n", m.n);
    int nf = 0;
    const char *e = dump(m.b, m.n, 0, &nf);
    CHECK(e == NULL);
    printf("dumped %d fields\n", nf);
    float f15; uint32_t u32 = 0x3fc00000u;
    memcpy(&f15, &u32, 4);
    double pi; uint64_t u64 = 0x400921fb54442d18ull;
    memcpy(&pi, &u64, 8);
    printf("fixed32 as float %.4f, fixed64 as double %.6f\n", (double)f15, pi);

    /* Zigzag table. */
    printf("zigzag:");
    static const int64_t zs[] = { 0, -1, 1, -2, 2147483647, -2147483648LL, 9223372036854775807LL };
    for (size_t i = 0; i < sizeof zs / sizeof zs[0]; i++) { CHECK(unzz64(zz64(zs[i])) == zs[i]); printf(" %lld->%llu", (long long)zs[i], (unsigned long long)zz64(zs[i])); }
    printf("\n");

    /* Malformed inputs. */
    struct { const char *label; size_t n; unsigned char b[16]; } bad[] = {
        { "truncated varint value", 2, { 0x08, 0x96 } },
        { "varint too long (11 bytes)", 12, { 0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 } },
        { "10th byte overflow", 11, { 0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02 } },
        { "length past end", 4, { 0x12, 0x05, 0x61, 0x62 } },
        { "field number zero", 2, { 0x00, 0x01 } },
        { "wire type 6", 2, { 0x0e, 0x01 } },
        { "truncated fixed32", 3, { 0x0d, 0x01, 0x02 } },
        { "non-canonical varint 1", 3, { 0x08, 0x81, 0x00 } },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int c = 0;
        printf("%s: ", bad[i].label);
        const char *r = dump(bad[i].b, bad[i].n, 0, &c);
        printf("%s\n", r ? r : "accepted");
    }
    /* Round trip random field lists through the parser. */
    long total = 0;
    for (int trial = 0; trial < 300; trial++) {
        W x = { { 0 }, 0 };
        struct { unsigned num, wt; uint64_t v; size_t len; unsigned char d[20]; } want[12];
        int nw = 1 + (int)(rnd() % 12);
        for (int i = 0; i < nw; i++) {
            unsigned num = 1 + rnd() % 2000;
            unsigned kind = rnd() % 4;
            uint64_t hi = rnd();
            uint64_t lo = rnd();
            uint64_t v = (hi << 32) | lo;
            v >>= rnd() % 64;
            want[i].num = num; want[i].v = v; want[i].len = 0;
            if (kind == 0) { want[i].wt = 0; f_varint(&x, num, v); }
            else if (kind == 1) { want[i].wt = 1; f_fixed64(&x, num, v); }
            else if (kind == 2) { want[i].wt = 5; want[i].v = (uint32_t)v; f_fixed32(&x, num, (uint32_t)v); }
            else { want[i].wt = 2; want[i].len = rnd() % 20; for (size_t k = 0; k < want[i].len; k++) want[i].d[k] = (unsigned char)rnd(); f_bytes(&x, num, want[i].d, want[i].len); }
        }
        const unsigned char *p = x.b, *end = x.b + x.n;
        for (int i = 0; i < nw; i++) {
            Field f;
            CHECK(next_field(&p, end, &f) == NULL);
            CHECK(f.num == want[i].num && f.wt == want[i].wt && f.canon);
            if (f.wt == 2) CHECK(f.len == want[i].len && memcmp(f.d, want[i].d, f.len) == 0);
            else CHECK(f.v == want[i].v);
        }
        CHECK(p == end);
        total += (long)x.n;
    }
    printf("300 random field lists round-trip (%ld bytes)\n", total);
    return 0;
}
