/*
 * title: Columnar file with row groups, delta and dictionary encoding, and footer index
 * topic: io_files
 * covers: column store, footer with offsets, magic at both ends, delta+zigzag varint, dictionary encoding, projection reads, min/max pruning
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

static uint32_t crc_tab[256];
void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
}
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n) {
    const unsigned char *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
uint32_t crc32_of(const void *buf, size_t n) { return crc32_update(0, buf, n); }

/* File: "CLM1" | column chunks... | footer | u32 footer_len | u32 footer_crc | "CLM1"
 * Footer per chunk: col, rowgroup, offset, length, rows, min, max, encoding.
 * Column 0 (id, int): delta + zigzag varint.  Column 1 (city, string): dictionary + 1-byte codes.
 * Column 2 (temp, int): plain zigzag varint. */
enum { ROWS = 300, RG = 100, NGROUPS = 3, NCOLS = 3 };

typedef struct { int col, rg; uint32_t off, len, rows; int64_t mn, mx; int enc; } Chunk;

static void pv(Buf *b, uint64_t v) { while (v >= 0x80) { bbyte(b, (unsigned)(v & 0x7F) | 0x80u); v >>= 7; } bbyte(b, (unsigned)v); }
static uint64_t zz(int64_t v) { return ((uint64_t)v << 1) ^ (uint64_t)(v < 0 ? -1 : 0); }
static int64_t unzz(uint64_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }
static uint64_t rv(const unsigned char *p, size_t *pos) {
    uint64_t v = 0;
    int sh = 0;
    for (;;) {
        unsigned char c = p[(*pos)++];
        v |= (uint64_t)(c & 0x7F) << sh;
        if (!(c & 0x80)) return v;
        sh += 7;
    }
}
static void p32(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) bbyte(b, (v >> (8 * i)) & 255u); }
static uint32_t g32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static const char *CITIES[] = {"Oslo", "Lima", "Kyiv", "Doha", "Baku", "Rome", "Nuuk"};

static int64_t g_id[ROWS], g_temp[ROWS];
static int g_city[ROWS];

int main(void) {
    crc_init();
    int64_t id = 1000;
    for (int i = 0; i < ROWS; i++) {
        id += 1 + (int64_t)(rnd() % 5);
        g_id[i] = id;
        g_city[i] = (int)(rnd() % 7);
        g_temp[i] = (int64_t)(rnd() % 700) - 300 + (i / RG) * 200;   /* later groups are warmer */
    }
    Buf f = {0};
    bstr(&f, "CLM1");
    Chunk ck[NCOLS * NGROUPS];
    int nck = 0;
    for (int rg = 0; rg < NGROUPS; rg++)
        for (int col = 0; col < NCOLS; col++) {
            Chunk *c = &ck[nck++];
            c->col = col; c->rg = rg; c->off = (uint32_t)f.n; c->rows = RG;
            int lo = rg * RG;
            int64_t mn = INT64_MAX, mx = INT64_MIN;
            if (col == 0) {
                c->enc = 1;
                int64_t prev = 0;
                for (int i = lo; i < lo + RG; i++) {
                    pv(&f, zz(g_id[i] - prev)); prev = g_id[i];
                    if (g_id[i] < mn) mn = g_id[i];
                    if (g_id[i] > mx) mx = g_id[i];
                }
            } else if (col == 1) {
                c->enc = 2;
                /* dictionary in first-seen order */
                int dict[7], nd = 0, code[RG];
                for (int i = lo; i < lo + RG; i++) {
                    int k;
                    for (k = 0; k < nd; k++) if (dict[k] == g_city[i]) break;
                    if (k == nd) dict[nd++] = g_city[i];
                    code[i - lo] = k;
                }
                bbyte(&f, (unsigned)nd);
                for (int k = 0; k < nd; k++) { bbyte(&f, (unsigned)strlen(CITIES[dict[k]])); bstr(&f, CITIES[dict[k]]); }
                for (int i = 0; i < RG; i++) bbyte(&f, (unsigned)code[i]);
                mn = mx = 0;
            } else {
                c->enc = 0;
                for (int i = lo; i < lo + RG; i++) {
                    pv(&f, zz(g_temp[i]));
                    if (g_temp[i] < mn) mn = g_temp[i];
                    if (g_temp[i] > mx) mx = g_temp[i];
                }
            }
            c->len = (uint32_t)f.n - c->off;
            c->mn = mn; c->mx = mx;
        }
    size_t fstart = f.n;
    p32(&f, (uint32_t)nck);
    for (int i = 0; i < nck; i++) {
        bbyte(&f, (unsigned)ck[i].col); bbyte(&f, (unsigned)ck[i].rg); bbyte(&f, (unsigned)ck[i].enc);
        p32(&f, ck[i].off); p32(&f, ck[i].len); p32(&f, ck[i].rows);
        p32(&f, (uint32_t)(int32_t)ck[i].mn); p32(&f, (uint32_t)(int32_t)ck[i].mx);
    }
    uint32_t flen = (uint32_t)(f.n - fstart);
    uint32_t fcrc = crc32_of(f.p + fstart, flen);
    p32(&f, flen); p32(&f, fcrc);
    bstr(&f, "CLM1");
    wfile("t.clm", f.p, f.n);
    size_t n;
    unsigned char *d = rfile("t.clm", &n);
    printf("file %zu bytes; column chunk sizes (id/city/temp): ", n);
    for (int rg = 0; rg < NGROUPS; rg++) printf("[%u %u %u] ", (unsigned)ck[rg * 3].len, (unsigned)ck[rg * 3 + 1].len, (unsigned)ck[rg * 3 + 2].len);
    printf("\nplain row-oriented size would be %d bytes\n", ROWS * (8 + 8 + 8));

    /* reader: load footer by seeking from the end */
    CHECK(!memcmp(d, "CLM1", 4) && !memcmp(d + n - 4, "CLM1", 4));
    uint32_t rfl = g32(d + n - 12), rfc = g32(d + n - 8);
    size_t rfs = n - 12 - rfl;
    CHECK(crc32_of(d + rfs, rfl) == rfc);
    uint32_t cnt = g32(d + rfs);
    CHECK(cnt == (uint32_t)nck);
    Chunk rc[16];
    for (uint32_t i = 0; i < cnt; i++) {
        const unsigned char *e = d + rfs + 4 + i * 23;
        rc[i].col = e[0]; rc[i].rg = e[1]; rc[i].enc = e[2];
        rc[i].off = g32(e + 3); rc[i].len = g32(e + 7); rc[i].rows = g32(e + 11);
        rc[i].mn = (int32_t)g32(e + 15); rc[i].mx = (int32_t)g32(e + 19);
    }
    /* projection: read only column 2 (temp) with predicate temp >= 400, pruning row groups by max */
    int matched = 0, groups_read = 0, groups_pruned = 0;
    int64_t sum = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (rc[i].col != 2) continue;
        if (rc[i].mx < 400) { groups_pruned++; continue; }
        groups_read++;
        size_t pos = rc[i].off;
        for (uint32_t r = 0; r < rc[i].rows; r++) {
            int64_t v = unzz(rv(d, &pos));
            if (v >= 400) { matched++; sum += v; }
        }
        CHECK(pos == rc[i].off + rc[i].len);
    }
    int exp_matched = 0;
    int64_t exp_sum = 0;
    for (int i = 0; i < ROWS; i++) if (g_temp[i] >= 400) { exp_matched++; exp_sum += g_temp[i]; }
    CHECK(matched == exp_matched && sum == exp_sum);
    printf("temp>=400: %d rows, sum %lld; row groups read=%d pruned=%d\n", matched, (long long)sum, groups_read, groups_pruned);
    /* city column of group 1: decode via dictionary and tally */
    for (uint32_t i = 0; i < cnt; i++) {
        if (rc[i].col != 1 || rc[i].rg != 1) continue;
        size_t pos = rc[i].off;
        int nd = d[pos++];
        char dict[7][8];
        for (int k = 0; k < nd; k++) { int l = d[pos++]; memcpy(dict[k], d + pos, (size_t)l); dict[k][l] = 0; pos += (size_t)l; }
        int tally[7] = {0};
        for (uint32_t r = 0; r < rc[i].rows; r++) {
            int code = d[pos++];
            CHECK(!strcmp(dict[code], CITIES[g_city[100 + (int)r]]));
            tally[code]++;
        }
        printf("group 1 dictionary (%d entries):", nd);
        for (int k = 0; k < nd; k++) printf(" %s=%d", dict[k], tally[k]);
        putchar('\n');
    }
    /* id column: delta decoding and lookup of an id range via min/max */
    int64_t want = g_id[250];
    for (uint32_t i = 0; i < cnt; i++) {
        if (rc[i].col != 0 || want < rc[i].mn || want > rc[i].mx) continue;
        size_t pos = rc[i].off;
        int64_t prev = 0;
        for (uint32_t r = 0; r < rc[i].rows; r++) {
            prev += unzz(rv(d, &pos));
            if (prev == want) printf("id %lld found in row group %d at row %d\n", (long long)want, rc[i].rg, rc[i].rg * RG + (int)r);
        }
    }
    /* footer corruption is caught by crc */
    d[rfs + 10] ^= 1;
    CHECK(crc32_of(d + rfs, rfl) != rfc);
    printf("footer corruption detected\n");
    free(d); bfree(&f);
    remove("t.clm");
    return 0;
}
