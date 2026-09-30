/*
 * title: HPACK integer, literal and table codec without Huffman
 * topic: networking
 * covers: HTTP/2 header compression, prefix integers, static table, dynamic table with eviction and size updates, incremental never-indexed and unindexed literals, RFC 7541 appendix C vectors
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { const char *n, *v; } SE;
static const SE stat[61] = {
    { ":authority", "" }, { ":method", "GET" }, { ":method", "POST" }, { ":path", "/" }, { ":path", "/index.html" }, { ":scheme", "http" },
    { ":scheme", "https" }, { ":status", "200" }, { ":status", "204" }, { ":status", "206" }, { ":status", "304" }, { ":status", "400" },
    { ":status", "404" }, { ":status", "500" }, { "accept-charset", "" }, { "accept-encoding", "gzip, deflate" }, { "accept-language", "" },
    { "accept-ranges", "" }, { "accept", "" }, { "access-control-allow-origin", "" }, { "age", "" }, { "allow", "" }, { "authorization", "" },
    { "cache-control", "" }, { "content-disposition", "" }, { "content-encoding", "" }, { "content-language", "" }, { "content-length", "" },
    { "content-location", "" }, { "content-range", "" }, { "content-type", "" }, { "cookie", "" }, { "date", "" }, { "etag", "" },
    { "expect", "" }, { "expires", "" }, { "from", "" }, { "host", "" }, { "if-match", "" }, { "if-modified-since", "" }, { "if-none-match", "" },
    { "if-range", "" }, { "if-unmodified-since", "" }, { "last-modified", "" }, { "link", "" }, { "location", "" }, { "max-forwards", "" },
    { "proxy-authenticate", "" }, { "proxy-authorization", "" }, { "range", "" }, { "referer", "" }, { "refresh", "" }, { "retry-after", "" },
    { "server", "" }, { "set-cookie", "" }, { "strict-transport-security", "" }, { "transfer-encoding", "" }, { "user-agent", "" },
    { "vary", "" }, { "via", "" }, { "www-authenticate", "" },
};

typedef struct { char n[64], v[128]; } DE;
typedef struct { DE e[64]; int cnt; size_t size, max; } Dyn; /* e[0] is newest */

static size_t esize(const DE *e) { return strlen(e->n) + strlen(e->v) + 32; }
static void dyn_evict(Dyn *d) {
    while (d->size > d->max && d->cnt > 0) { d->size -= esize(&d->e[d->cnt - 1]); d->cnt--; }
}
static void dyn_add(Dyn *d, const char *n, const char *v) {
    DE ne;
    snprintf(ne.n, sizeof ne.n, "%s", n);
    snprintf(ne.v, sizeof ne.v, "%s", v);
    size_t s = esize(&ne);
    if (s > d->max) { d->cnt = 0; d->size = 0; return; }
    d->size += s;
    dyn_evict(d);
    while (d->cnt >= 64) { d->size -= esize(&d->e[d->cnt - 1]); d->cnt--; }
    memmove(&d->e[1], &d->e[0], (size_t)d->cnt * sizeof(DE));
    d->e[0] = ne;
    d->cnt++;
}
static int lookup(const Dyn *d, unsigned idx, const char **n, const char **v) {
    if (idx == 0) return 0;
    if (idx <= 61) { *n = stat[idx - 1].n; *v = stat[idx - 1].v; return 1; }
    idx -= 62;
    if (idx >= (unsigned)d->cnt) return 0;
    *n = d->e[idx].n; *v = d->e[idx].v;
    return 1;
}

/* 5.1 integer with an N bit prefix; first byte's high bits come from `pat`. */
static size_t put_int(unsigned char *o, unsigned pat, int nbits, uint32_t v) {
    uint32_t max = (1u << nbits) - 1;
    if (v < max) { o[0] = (unsigned char)(pat | v); return 1; }
    size_t n = 0;
    o[n++] = (unsigned char)(pat | max);
    v -= max;
    while (v >= 128) { o[n++] = (unsigned char)((v & 127) | 128); v >>= 7; }
    o[n++] = (unsigned char)v;
    return n;
}
static int get_int(const unsigned char *p, size_t n, int nbits, uint32_t *v, size_t *used) {
    if (n == 0) return 0;
    uint32_t max = (1u << nbits) - 1;
    uint32_t x = p[0] & max;
    size_t i = 1;
    if (x == max) {
        unsigned shift = 0;
        for (;;) {
            if (i >= n) return 0;
            if (shift > 28) return 0;
            unsigned b = p[i++];
            x += (uint32_t)(b & 127) << shift;
            if (!(b & 128)) break;
            shift += 7;
        }
    }
    *v = x; *used = i;
    return 1;
}

static size_t put_str(unsigned char *o, const char *s) {
    size_t l = strlen(s);
    size_t n = put_int(o, 0, 7, (uint32_t)l);
    memcpy(o + n, s, l);
    return n + l;
}

/* Encoder: indexed if exact match, else literal with incremental indexing (indexed name when possible).
   sensitive names are sent never-indexed. */
static size_t encode_field(Dyn *d, unsigned char *o, const char *n, const char *v, int sensitive) {
    int name_idx = 0;
    for (int i = 1; i <= 61 + d->cnt; i++) {
        const char *sn, *sv;
        lookup(d, (unsigned)i, &sn, &sv);
        if (strcmp(sn, n) == 0) {
            if (strcmp(sv, v) == 0 && !sensitive && !(i <= 61 && v[0] == 0 && stat[i - 1].v[0] == 0 && 0)) return put_int(o, 0x80, 7, (unsigned)i);
            if (!name_idx) name_idx = i;
        }
    }
    size_t k;
    if (sensitive) k = put_int(o, 0x10, 4, (unsigned)name_idx);
    else k = put_int(o, 0x40, 6, (unsigned)name_idx);
    if (!name_idx) k += put_str(o + k, n);
    k += put_str(o + k, v);
    if (!sensitive) dyn_add(d, n, v);
    return k;
}

typedef struct { char n[64], v[128]; } HF;

static const char *decode_block(Dyn *d, const unsigned char *p, size_t n, HF *out, int *cnt) {
    size_t i = 0;
    *cnt = 0;
    while (i < n) {
        unsigned b = p[i];
        uint32_t v;
        size_t u;
        const char *nm = NULL, *vl = NULL;
        char nbuf[64], vbuf[128];
        if (b & 0x80) {
            if (!get_int(p + i, n - i, 7, &v, &u)) return "truncated-int";
            i += u;
            if (!lookup(d, v, &nm, &vl)) return "bad-index";
        } else if ((b & 0xe0) == 0x20) {
            if (!get_int(p + i, n - i, 5, &v, &u)) return "truncated-int";
            i += u;
            if (v > 4096) return "table-size-too-large";
            d->max = v;
            dyn_evict(d);
            continue;
        } else {
            int prefix = (b & 0x40) ? 6 : 4;
            int incremental = (b & 0x40) != 0;
            if (!get_int(p + i, n - i, prefix, &v, &u)) return "truncated-int";
            i += u;
            if (v) {
                if (!lookup(d, v, &nm, &vl)) return "bad-index";
                snprintf(nbuf, sizeof nbuf, "%s", nm);
            } else {
                if (i >= n) return "truncated-string";
                if (p[i] & 0x80) return "huffman-unsupported";
                uint32_t l;
                if (!get_int(p + i, n - i, 7, &l, &u)) return "truncated-int";
                i += u;
                if (i + l > n) return "truncated-string";
                if (l >= sizeof nbuf) return "name-too-long";
                memcpy(nbuf, p + i, l); nbuf[l] = 0; i += l;
            }
            if (i >= n) return "truncated-string";
            if (p[i] & 0x80) return "huffman-unsupported";
            uint32_t l;
            if (!get_int(p + i, n - i, 7, &l, &u)) return "truncated-int";
            i += u;
            if (i + l > n) return "truncated-string";
            if (l >= sizeof vbuf) return "value-too-long";
            memcpy(vbuf, p + i, l); vbuf[l] = 0; i += l;
            nm = nbuf; vl = vbuf;
            if (incremental) dyn_add(d, nbuf, vbuf);
        }
        snprintf(out[*cnt].n, 64, "%s", nm);
        snprintf(out[*cnt].v, 128, "%s", vl);
        (*cnt)++;
    }
    return NULL;
}

static void hex(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x%s", p[i], (i % 2 && i + 1 < n) ? " " : "");
    printf("\n");
}

static void show_table(const Dyn *d) {
    printf("  dynamic table (%zu bytes, max %zu):", d->size, d->max);
    for (int i = 0; i < d->cnt; i++) printf(" [%d]%s", i + 1, d->e[i].n);
    printf("\n");
}

static uint32_t rs = 0x4a7a4a7au;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    unsigned char b[64];
    /* RFC 7541 section C.1 integer examples. */
    size_t k = put_int(b, 0, 5, 10);
    printf("10 in 5 bits: "); hex(b, k);
    CHECK(k == 1 && b[0] == 0x0a);
    k = put_int(b, 0, 5, 1337);
    printf("1337 in 5 bits: "); hex(b, k);
    CHECK(k == 3 && b[0] == 0x1f && b[1] == 0x9a && b[2] == 0x0a);
    k = put_int(b, 0, 8, 42);
    printf("42 in 8 bits: "); hex(b, k);
    CHECK(k == 1 && b[0] == 42);
    uint32_t bad_v; size_t bu;
    static const unsigned char ovf[] = { 0x1f, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
    CHECK(!get_int(ovf, sizeof ovf, 5, &bad_v, &bu));
    for (uint32_t v = 0; v < 70000; v += (v < 300 ? 1 : 977)) {
        for (int nb = 1; nb <= 8; nb++) {
            unsigned char t[8];
            size_t l = put_int(t, 0, nb, v);
            uint32_t back; size_t u;
            CHECK(get_int(t, l, nb, &back, &u) && back == v && u == l);
        }
    }
    printf("integer round trips ok for prefixes 1..8 bits\n");

    /* C.2 single fields. */
    Dyn d = { .max = 4096 };
    k = encode_field(&d, b, "custom-key", "custom-header", 0);
    printf("C.2.1 literal+index: "); hex(b, k);
    static const unsigned char c21[] = { 0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0d, 'c', 'u', 's', 't', 'o', 'm', '-', 'h', 'e', 'a', 'd', 'e', 'r' };
    CHECK(k == sizeof c21 && memcmp(b, c21, k) == 0 && d.size == 55);
    Dyn d2 = { .max = 4096 };
    k = encode_field(&d2, b, "password", "secret", 1);
    printf("C.2.3 never indexed: "); hex(b, k);
    static const unsigned char c23[] = { 0x10, 0x08, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', 0x06, 's', 'e', 'c', 'r', 'e', 't' };
    CHECK(k == sizeof c23 && memcmp(b, c23, k) == 0 && d2.cnt == 0);
    Dyn d3 = { .max = 4096 };
    k = encode_field(&d3, b, ":method", "GET", 0);
    CHECK(k == 1 && b[0] == 0x82);

    /* C.3 request sequence and C.5 response sequence, encoder and decoder tables must agree. */
    static const char *reqs[3][6][2] = {
        { { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" }, { 0, 0 } },
        { { ":method", "GET" }, { ":scheme", "http" }, { ":path", "/" }, { ":authority", "www.example.com" }, { "cache-control", "no-cache" }, { 0, 0 } },
        { { ":method", "GET" }, { ":scheme", "https" }, { ":path", "/index.html" }, { ":authority", "www.example.com" }, { "custom-key", "custom-value" }, { 0, 0 } },
    };
    static const char *want3[3] = {
        "828684410f7777772e6578616d706c652e636f6d",
        "828684be58086e6f2d6361636865",
        "828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565",
    };
    Dyn enc = { .max = 4096 }, dec = { .max = 4096 };
    for (int r = 0; r < 3; r++) {
        unsigned char msg[256];
        size_t n = 0;
        for (int f = 0; reqs[r][f][0]; f++) n += encode_field(&enc, msg + n, reqs[r][f][0], reqs[r][f][1], 0);
        char h[600];
        for (size_t i = 0; i < n; i++) snprintf(h + 2 * i, 3, "%02x", msg[i]);
        printf("C.3.%d request (%zu bytes): %s\n", r + 1, n, h);
        CHECK(strcmp(h, want3[r]) == 0);
        HF hf[8]; int cnt;
        CHECK(decode_block(&dec, msg, n, hf, &cnt) == NULL);
        for (int f = 0; f < cnt; f++) CHECK(strcmp(hf[f].n, reqs[r][f][0]) == 0 && strcmp(hf[f].v, reqs[r][f][1]) == 0);
        CHECK(enc.size == dec.size && enc.cnt == dec.cnt);
        show_table(&dec);
    }
    static const char *resp[3][7][2] = {
        { { ":status", "302" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "location", "https://www.example.com" }, { 0, 0 } },
        { { ":status", "307" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:21 GMT" }, { "location", "https://www.example.com" }, { 0, 0 } },
        { { ":status", "200" }, { "cache-control", "private" }, { "date", "Mon, 21 Oct 2013 20:13:22 GMT" }, { "location", "https://www.example.com" },
          { "content-encoding", "gzip" }, { "set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1" }, { 0, 0 } },
    };
    static const size_t want5len[3] = { 70, 8, 98 };
    Dyn e5 = { .max = 256 }, d5 = { .max = 256 };
    for (int r = 0; r < 3; r++) {
        unsigned char msg[256];
        size_t n = 0;
        for (int f = 0; resp[r][f][0]; f++) n += encode_field(&e5, msg + n, resp[r][f][0], resp[r][f][1], 0);
        printf("C.5.%d response: %zu bytes, first bytes ", r + 1, n);
        hex(msg, r == 1 ? n : 8);
        CHECK(n == want5len[r]);
        HF hf[8]; int cnt;
        CHECK(decode_block(&d5, msg, n, hf, &cnt) == NULL);
        for (int f = 0; f < cnt; f++) CHECK(strcmp(hf[f].n, resp[r][f][0]) == 0 && strcmp(hf[f].v, resp[r][f][1]) == 0);
        CHECK(e5.size == d5.size && e5.cnt == d5.cnt);
        show_table(&d5);
    }
    CHECK(d5.size == 215 && d5.cnt == 3);

    /* Random header lists, several table sizes, plus a mid-stream size update. */
    static const char *names[] = { "x-a", "x-bb", "cookie", "user-agent", "accept", ":path", "x-trace-id", "content-type" };
    for (size_t maxsz = 0; maxsz <= 4096; maxsz = maxsz ? maxsz * 4 : 64) {
        Dyn e = { .max = maxsz }, dd = { .max = maxsz };
        long bytes = 0, raw = 0;
        for (int msg = 0; msg < 200; msg++) {
            unsigned char wire[1200];
            size_t n = 0;
            HF want[6];
            int nf = 1 + (int)(rnd() % 5);
            for (int f = 0; f < nf; f++) {
                uint32_t r1 = rnd();
                uint32_t r2 = rnd();
                snprintf(want[f].n, 64, "%s", names[r1 % 8]);
                snprintf(want[f].v, 128, "v%u", (unsigned)(r2 % 6));
                raw += (long)(strlen(want[f].n) + strlen(want[f].v));
                n += encode_field(&e, wire + n, want[f].n, want[f].v, (r2 >> 8) % 11 == 0);
            }
            HF got[8]; int cnt;
            CHECK(decode_block(&dd, wire, n, got, &cnt) == NULL && cnt == nf);
            for (int f = 0; f < nf; f++) CHECK(strcmp(got[f].n, want[f].n) == 0 && strcmp(got[f].v, want[f].v) == 0);
            CHECK(e.size == dd.size && e.cnt == dd.cnt);
            bytes += (long)n;
        }
        printf("table max %4zu: 200 blocks, %ld raw header bytes -> %ld encoded\n", maxsz, raw, bytes);
    }
    /* Decoder rejections. */
    Dyn dz = { .max = 4096 };
    HF hf[4]; int cnt;
    static const unsigned char e1[] = { 0xff, 0x80, 0x80 }, e2[] = { 0x80 }, e3[] = { 0x40, 0x83, 'a', 'b', 'c', 0x01, 'x' }, e4[] = { 0x00, 0x05, 'a' }, e5b[] = { 0x3f, 0xe2, 0x1f };
    const char *r1 = decode_block(&dz, e1, sizeof e1, hf, &cnt);
    const char *r2 = decode_block(&dz, e2, sizeof e2, hf, &cnt);
    const char *r3 = decode_block(&dz, e3, sizeof e3, hf, &cnt);
    const char *r4 = decode_block(&dz, e4, sizeof e4, hf, &cnt);
    const char *r5 = decode_block(&dz, e5b, sizeof e5b, hf, &cnt);
    printf("errors: %s %s %s %s %s\n", r1, r2, r3, r4, r5);
    return 0;
}
