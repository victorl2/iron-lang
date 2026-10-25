/*
 * title: Length-prefixed CRC record log with corruption recovery
 * topic: io_files
 * covers: record framing, crc32, torn tail write, bit flips, resync via magic, recovery report, append after truncate
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

/* Record: magic 0xA5 0x5A | len u16 | seq u32 | payload | crc32(len,seq,payload) */
enum { MAGIC0 = 0xA5, MAGIC1 = 0x5A, HDR = 8 };

static void put32(Buf *b, uint32_t v) { for (int i = 0; i < 4; i++) bbyte(b, (v >> (8 * i)) & 255u); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void append_rec(Buf *b, uint32_t seq, const unsigned char *pl, unsigned len) {
    size_t start = b->n;
    bbyte(b, MAGIC0); bbyte(b, MAGIC1);
    bbyte(b, len & 255u); bbyte(b, len >> 8);
    put32(b, seq);
    bput(b, pl, len);
    uint32_t c = crc32_of(b->p + start + 2, b->n - start - 2);
    put32(b, c);
}

typedef struct { int good, bad_crc, resyncs, torn; size_t skipped, valid_end; uint32_t seqs[64]; } Report;

/* Scan; on a bad record skip one byte and look for the next magic.
 * A record that runs past the end is only "torn" if nothing valid follows it. */
static void scan(const unsigned char *d, size_t n, Report *r) {
    memset(r, 0, sizeof *r);
    size_t p = 0;
    int in_garbage = 0;
    while (p < n) {
        if (d[p] == MAGIC0 && p + HDR + 4 <= n && d[p + 1] == MAGIC1) {
            unsigned len = (unsigned)d[p + 2] | ((unsigned)d[p + 3] << 8);
            if (p + HDR + len + 4 > n) {
                r->torn = 1;
            } else if (crc32_of(d + p + 2, HDR - 2 + len) == get32(d + p + HDR + len)) {
                if (r->good < 64) r->seqs[r->good] = get32(d + p + 4);
                r->good++;
                p += HDR + len + 4;
                r->valid_end = p;
                r->torn = 0;
                in_garbage = 0;
                continue;
            } else r->bad_crc++;
        }
        if (!in_garbage) { r->resyncs++; in_garbage = 1; }
        r->skipped++;
        p++;
    }
}

static void show(const char *what, const Report *r) {
    printf("%-22s good=%2d bad_crc=%d resyncs=%d torn=%d skipped=%3zu valid_end=%zu\n", what, r->good, r->bad_crc, r->resyncs, r->torn,
           r->skipped, r->valid_end);
}

int main(void) {
    crc_init();
    Buf log = {0};
    unsigned char pl[200];
    size_t ends[16];
    for (uint32_t i = 0; i < 12; i++) {
        unsigned len = (unsigned)(rnd() % 40);
        for (unsigned k = 0; k < len; k++) pl[k] = (unsigned char)(rnd() >> 7);
        append_rec(&log, i, pl, len);
        ends[i] = log.n;
    }
    wfile("wal.log", log.p, log.n);
    size_t n;
    unsigned char *d = rfile("wal.log", &n);
    Report r;
    scan(d, n, &r);
    CHECK(r.good == 12 && !r.bad_crc && !r.torn && r.valid_end == n);
    show("clean log", &r);
    printf("log size %zu bytes\n", n);

    /* flip one payload bit in record 4 */
    unsigned char *c = malloc(n + 64);
    memcpy(c, d, n);
    size_t mid = (ends[3] + ends[4]) / 2;
    c[mid] ^= 0x08;
    scan(c, n, &r);
    CHECK(r.good == 11 && r.bad_crc >= 1);
    show("bit flip in rec 4", &r);
    for (int i = 0; i < r.good; i++) CHECK(r.seqs[i] != 4);

    /* flip a magic byte: record is lost, neighbours survive */
    memcpy(c, d, n);
    c[ends[6]] ^= 0xFF;
    scan(c, n, &r);
    CHECK(r.good == 11);
    show("magic wrecked rec 7", &r);

    /* torn tail: last record cut in half */
    size_t cut = (ends[10] + ends[11]) / 2;
    scan(d, cut, &r);
    CHECK(r.good == 11 && r.torn && r.valid_end == ends[10]);
    show("torn tail", &r);

    /* recover: truncate at valid_end, append a new record, rescan */
    size_t keep = r.valid_end;
    memcpy(c, d, keep);
    Buf tail = {0};
    append_rec(&tail, 99, (const unsigned char *)"after recovery", 14);
    memcpy(c + keep, tail.p, tail.n);
    wfile("wal.log", c, keep + tail.n);
    free(d);
    d = rfile("wal.log", &n);
    scan(d, n, &r);
    CHECK(r.good == 12 && r.seqs[11] == 99 && !r.torn);
    show("after truncate+append", &r);

    /* payload that embeds the magic bytes must not confuse the scanner */
    Buf tricky = {0};
    unsigned char fake[64];
    memset(fake, 0, sizeof fake);
    Buf inner = {0};
    append_rec(&inner, 7, (const unsigned char *)"inner", 5);
    memcpy(fake, inner.p, inner.n);
    append_rec(&tricky, 1, fake, (unsigned)inner.n);
    append_rec(&tricky, 2, (const unsigned char *)"tail", 4);
    scan(tricky.p, tricky.n, &r);
    CHECK(r.good == 2 && r.seqs[0] == 1 && r.seqs[1] == 2);
    show("embedded record", &r);
    /* damage the outer record: the embedded one is now found as a stray valid record */
    tricky.p[4] ^= 0x01;
    scan(tricky.p, tricky.n, &r);
    show("outer damaged", &r);
    CHECK(r.good == 2 && r.seqs[0] == 7 && r.seqs[1] == 2);

    /* random single-bit flips: exactly one record is ever lost, never a false accept */
    int lost_hist[4] = {0};
    for (int t = 0; t < 200; t++) {
        memcpy(c, d, n);
        size_t at = rnd() % n;
        c[at] ^= (unsigned char)(1u << (rnd() % 8));
        scan(c, n, &r);
        int lost = 12 - r.good;
        CHECK(lost >= 0 && lost <= 1);
        lost_hist[lost]++;
        for (int i = 0; i < r.good && i < 64; i++) CHECK(r.seqs[i] <= 99);
    }
    printf("200 random bit flips, records lost: 0->%d 1->%d\n", lost_hist[0], lost_hist[1]);
    free(c); free(d); bfree(&log); bfree(&tail); bfree(&tricky); bfree(&inner);
    remove("wal.log");
    return 0;
}
