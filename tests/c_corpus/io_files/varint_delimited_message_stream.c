/*
 * title: Varint length-delimited message stream with zigzag fields
 * topic: io_files
 * covers: protobuf-style wire format, varint, zigzag, field tags, wire types, nested messages, unknown field skipping, overlong varint rejection
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

/* Message: field 1 varint id, field 2 zigzag delta (sint), field 3 bytes name, field 4 fixed32, field 5 nested Point{1:x sint,2:y sint}.
 * Stream: repeated [varint length][message bytes]. */
enum { WT_VARINT = 0, WT_64 = 1, WT_LEN = 2, WT_32 = 5 };

static void put_varint(Buf *b, uint64_t v) {
    while (v >= 0x80) { bbyte(b, (unsigned)(v & 0x7F) | 0x80u); v >>= 7; }
    bbyte(b, (unsigned)v);
}
static uint64_t zz(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v < 0 ? -1 : 0); }
static int64_t unzz(uint64_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }
static void put_tag(Buf *b, unsigned field, unsigned wt) { put_varint(b, (field << 3) | wt); }

/* returns bytes consumed or 0 on error (truncated, >10 bytes, or overlong final byte) */
static size_t get_varint(const unsigned char *p, size_t n, uint64_t *out) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && i < 10; i++) {
        uint64_t part = p[i] & 0x7Fu;
        if (i == 9 && part > 1) return 0;               /* would overflow 64 bits */
        v |= part << (7 * i);
        if (!(p[i] & 0x80u)) {
            if (i > 0 && p[i] == 0) return 0;           /* non-canonical: trailing zero group */
            *out = v;
            return i + 1;
        }
    }
    return 0;
}

typedef struct {
    uint64_t id; int64_t delta; char name[24]; uint32_t f32; int has_point; int64_t px, py; int unknown;
} Msg;

static void encode_msg(Buf *out, const Msg *m, int with_unknown) {
    Buf body = {0};
    put_tag(&body, 1, WT_VARINT); put_varint(&body, m->id);
    put_tag(&body, 2, WT_VARINT); put_varint(&body, zz(m->delta));
    put_tag(&body, 3, WT_LEN); put_varint(&body, strlen(m->name)); bstr(&body, m->name);
    put_tag(&body, 4, WT_32);
    for (int i = 0; i < 4; i++) bbyte(&body, (m->f32 >> (8 * i)) & 255u);
    if (m->has_point) {
        Buf pt = {0};
        put_tag(&pt, 1, WT_VARINT); put_varint(&pt, zz(m->px));
        put_tag(&pt, 2, WT_VARINT); put_varint(&pt, zz(m->py));
        put_tag(&body, 5, WT_LEN); put_varint(&body, pt.n); bput(&body, pt.p, pt.n);
        bfree(&pt);
    }
    if (with_unknown) {
        put_tag(&body, 15, WT_64);
        for (int i = 0; i < 8; i++) bbyte(&body, 0xAA);
        put_tag(&body, 9, WT_LEN); put_varint(&body, 3); bstr(&body, "xyz");
        put_tag(&body, 12, WT_VARINT); put_varint(&body, 300);
    }
    put_varint(out, body.n);
    bput(out, body.p, body.n);
    bfree(&body);
}

static int decode_msg(const unsigned char *p, size_t n, Msg *m, int depth);

static int decode_point(const unsigned char *p, size_t n, Msg *m) {
    size_t i = 0;
    while (i < n) {
        uint64_t tag, v;
        size_t k = get_varint(p + i, n - i, &tag);
        if (!k) return 0;
        i += k;
        if ((tag >> 3) == 1 && (tag & 7) == WT_VARINT) { k = get_varint(p + i, n - i, &v); if (!k) return 0; m->px = unzz(v); i += k; }
        else if ((tag >> 3) == 2 && (tag & 7) == WT_VARINT) { k = get_varint(p + i, n - i, &v); if (!k) return 0; m->py = unzz(v); i += k; }
        else return 0;
    }
    m->has_point = 1;
    return 1;
}

static int decode_msg(const unsigned char *p, size_t n, Msg *m, int depth) {
    size_t i = 0;
    (void)depth;
    memset(m, 0, sizeof *m);
    while (i < n) {
        uint64_t tag, v;
        size_t k = get_varint(p + i, n - i, &tag);
        if (!k) return 0;
        i += k;
        unsigned field = (unsigned)(tag >> 3), wt = (unsigned)(tag & 7);
        switch (wt) {
        case WT_VARINT:
            k = get_varint(p + i, n - i, &v);
            if (!k) return 0;
            i += k;
            if (field == 1) m->id = v; else if (field == 2) m->delta = unzz(v); else m->unknown++;
            break;
        case WT_64:
            if (n - i < 8) return 0;
            i += 8; m->unknown++;
            break;
        case WT_32:
            if (n - i < 4) return 0;
            if (field == 4) m->f32 = (uint32_t)p[i] | ((uint32_t)p[i + 1] << 8) | ((uint32_t)p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24);
            else m->unknown++;
            i += 4;
            break;
        case WT_LEN:
            k = get_varint(p + i, n - i, &v);
            if (!k) return 0;
            i += k;
            if (v > n - i) return 0;
            if (field == 3) { if (v >= sizeof m->name) return 0; memcpy(m->name, p + i, (size_t)v); m->name[v] = 0; }
            else if (field == 5) { if (!decode_point(p + i, (size_t)v, m)) return 0; }
            else m->unknown++;
            i += (size_t)v;
            break;
        default: return 0;
        }
    }
    return 1;
}

int main(void) {
    static const char *names[] = {"", "a", "sensor-alpha", "beta", "a-rather-long-name"};
    Msg src[20];
    Buf stream = {0};
    for (int i = 0; i < 20; i++) {
        memset(&src[i], 0, sizeof src[i]);
        uint32_t r1 = rnd(), r2 = rnd(), r3 = rnd();
        src[i].id = i % 4 == 0 ? ((uint64_t)r1 << 32 | r2) : (uint64_t)(r1 % (1u << (1 + (unsigned)(i % 30))));
        src[i].delta = (int64_t)(int32_t)r2 / (i % 3 ? 1 : 1000);
        snprintf(src[i].name, sizeof src[i].name, "%s", names[r3 % 5]);
        src[i].f32 = r3 * 2654435761u;
        src[i].has_point = i % 3 != 1;
        src[i].px = (int64_t)(int16_t)r1;
        src[i].py = -(int64_t)(r2 & 0xFFFF);
        if (!src[i].has_point) src[i].px = src[i].py = 0;
        encode_msg(&stream, &src[i], i % 5 == 4);
    }
    wfile("msgs.bin", stream.p, stream.n);
    size_t n;
    unsigned char *d = rfile("msgs.bin", &n);
    size_t pos = 0;
    int idx = 0, unknown_total = 0;
    while (pos < n) {
        uint64_t len;
        size_t k = get_varint(d + pos, n - pos, &len);
        CHECK(k && len <= n - pos - k);
        pos += k;
        Msg m;
        CHECK(decode_msg(d + pos, (size_t)len, &m, 0));
        pos += (size_t)len;
        CHECK(m.id == src[idx].id && m.delta == src[idx].delta && !strcmp(m.name, src[idx].name));
        CHECK(m.f32 == src[idx].f32 && m.has_point == src[idx].has_point && m.px == src[idx].px && m.py == src[idx].py);
        unknown_total += m.unknown;
        if (idx < 4) printf("msg %d: len=%2llu id=%llu delta=%lld name=\"%s\" f32=%08X point=%s(%lld,%lld) unknown=%d\n", idx,
                            (unsigned long long)len, (unsigned long long)m.id, (long long)m.delta, m.name, (unsigned)m.f32,
                            m.has_point ? "" : "none", (long long)m.px, (long long)m.py, m.unknown);
        idx++;
    }
    CHECK(idx == 20);
    printf("stream: %zu bytes, %d messages, %d unknown fields skipped\n", n, idx, unknown_total);
    /* varint edge values and encoded sizes */
    static const uint64_t vals[] = {0, 1, 127, 128, 16383, 16384, 0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
    for (int i = 0; i < 9; i++) {
        Buf b = {0};
        put_varint(&b, vals[i]);
        uint64_t back;
        CHECK(get_varint(b.p, b.n, &back) == b.n && back == vals[i]);
        printf("varint %20llu -> %2zu bytes, last=%02X\n", (unsigned long long)vals[i], b.n, b.p[b.n - 1]);
        bfree(&b);
    }
    static const int64_t sv[] = {0, -1, 1, -2, 2147483647, -2147483647 - 1};
    printf("zigzag:");
    for (int i = 0; i < 6; i++) { CHECK(unzz(zz(sv[i])) == sv[i]); printf(" %lld->%llu", (long long)sv[i], (unsigned long long)zz(sv[i])); }
    putchar('\n');
    /* malformed varints */
    static const unsigned char bad1[] = {0x80, 0x80};
    static const unsigned char bad2[] = {0x81, 0x00};
    static const unsigned char bad3[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    static const unsigned char bad4[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    uint64_t t;
    printf("truncated=%zu overlong=%zu overflow=%zu too_long=%zu\n", get_varint(bad1, 2, &t), get_varint(bad2, 2, &t),
           get_varint(bad3, 10, &t), get_varint(bad4, 11, &t));
    /* truncate the stream at every byte: decoder must never accept a partial message as complete garbage */
    int clean_cuts = 0;
    for (size_t cut = 0; cut < n; cut++) {
        size_t q = 0;
        int ok = 1;
        while (q < cut) {
            uint64_t len;
            size_t k = get_varint(d + q, cut - q, &len);
            if (!k || len > cut - q - k) { ok = 0; break; }
            q += k + (size_t)len;
        }
        if (ok) clean_cuts++;
    }
    printf("clean cut points: %d of %zu (message boundaries plus empty)\n", clean_cuts, n);
    CHECK(clean_cuts == 20);
    free(d); bfree(&stream);
    remove("msgs.bin");
    return 0;
}
