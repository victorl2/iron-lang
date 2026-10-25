/*
 * title: Byte ring buffer for streaming with contiguous spans
 * topic: data_structures
 * covers: byte ring buffer, partial read/write, wraparound memcpy, peek, discard, contiguous span views
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xA4093822299F31D0ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 27); }

#define CAP 64
typedef struct { unsigned char buf[CAP]; size_t head, len; long split_writes, split_reads; } Ring;

static size_t r_free(const Ring *r) { return CAP - r->len; }
/* write up to n bytes, return how many were accepted */
static size_t r_write(Ring *r, const unsigned char *src, size_t n) {
    if (n > r_free(r)) n = r_free(r);
    size_t tail = (r->head + r->len) % CAP, first = CAP - tail;
    if (first > n) first = n;
    memcpy(r->buf + tail, src, first);
    if (n > first) { memcpy(r->buf, src + first, n - first); r->split_writes++; }
    r->len += n; return n;
}
static size_t r_peek(const Ring *r, unsigned char *dst, size_t n, size_t skip) {
    if (skip >= r->len) return 0;
    if (n > r->len - skip) n = r->len - skip;
    size_t start = (r->head + skip) % CAP, first = CAP - start;
    if (first > n) first = n;
    memcpy(dst, r->buf + start, first);
    if (n > first) memcpy(dst + first, r->buf, n - first);
    return n;
}
static size_t r_read(Ring *r, unsigned char *dst, size_t n) {
    n = r_peek(r, dst, n, 0);
    if (r->head + n > CAP) r->split_reads++;
    r->head = (r->head + n) % CAP; r->len -= n;
    return n;
}
static size_t r_discard(Ring *r, size_t n) { if (n > r->len) n = r->len; r->head = (r->head + n) % CAP; r->len -= n; return n; }
/* the longest readable run without wrapping */
static size_t r_contig_read(const Ring *r, const unsigned char **p) { *p = r->buf + r->head; size_t c = CAP - r->head; return r->len < c ? r->len : c; }
static size_t r_contig_write(Ring *r, unsigned char **p) { size_t t = (r->head + r->len) % CAP; *p = r->buf + t; size_t c = CAP - t; return r_free(r) < c ? r_free(r) : c; }
/* find a byte in the buffered data; returns the offset or -1 */
static long r_find(const Ring *r, unsigned char c) {
    for (size_t i = 0; i < r->len; i++) if (r->buf[(r->head + i) % CAP] == c) return (long)i;
    return -1;
}

int main(void) {
    Ring r; memset(&r, 0, sizeof r);
    static unsigned char model[1 << 16]; size_t mh = 0, mt = 0;
    unsigned char seq = 0;
    long written = 0, read_ = 0, discarded = 0, lines = 0, zero_copy = 0, rejected = 0;
    unsigned long long crc = 1469598103934665603ULL;
    for (int step = 0; step < 30000; step++) {
        unsigned op = rnd() % 100;
        if (mt > 60000) { memmove(model, model + mh, mt - mh); mt -= mh; mh = 0; }
        if (op < 35) {
            unsigned char tmp[40]; size_t n = 1 + rnd() % 40;
            for (size_t i = 0; i < n; i++) tmp[i] = (unsigned char)(rnd() % 20 == 0 ? '\n' : seq++);
            size_t w = r_write(&r, tmp, n);
            CHECK(w == (n < CAP - (mt - mh) ? n : CAP - (mt - mh)));
            if (w < n) rejected++;
            memcpy(model + mt, tmp, w); mt += w; written += (long)w;
        } else if (op < 60) {
            unsigned char tmp[40]; size_t n = 1 + rnd() % 40;
            size_t got = r_read(&r, tmp, n);
            size_t want = n < mt - mh ? n : mt - mh;
            CHECK(got == want && memcmp(tmp, model + mh, got) == 0);
            for (size_t i = 0; i < got; i++) crc = (crc ^ tmp[i]) * 1099511628211ULL;
            mh += got; read_ += (long)got;
        } else if (op < 70) {
            size_t n = rnd() % 30; size_t d = r_discard(&r, n);
            CHECK(d == (n < mt - mh ? n : mt - mh)); mh += d; discarded += (long)d;
        } else if (op < 80) {
            unsigned char tmp[20]; size_t skip = rnd() % 30, n = rnd() % 20;
            size_t got = r_peek(&r, tmp, n, skip);
            size_t avail = mt - mh; size_t want = skip >= avail ? 0 : (n < avail - skip ? n : avail - skip);
            CHECK(got == want && memcmp(tmp, model + mh + skip, got) == 0);
        } else if (op < 90) {
            const unsigned char *p; size_t c = r_contig_read(&r, &p);
            CHECK(c <= r.len && memcmp(p, model + mh, c) == 0);
            size_t take = c / 2; r_discard(&r, take); mh += take; discarded += (long)take; zero_copy++;
        } else if (op < 95) {
            unsigned char *p; size_t c = r_contig_write(&r, &p);
            size_t n = c < 5 ? c : 5;
            for (size_t i = 0; i < n; i++) { p[i] = (unsigned char)(seq++); model[mt + i] = p[i]; }
            r.len += n; mt += n; written += (long)n; zero_copy++;
        } else {
            long f = r_find(&r, '\n'), g = -1;
            for (size_t i = mh; i < mt; i++) if (model[i] == '\n') { g = (long)(i - mh); break; }
            CHECK(f == g);
            if (f >= 0) { unsigned char line[CAP]; size_t got = r_read(&r, line, (size_t)f + 1); CHECK(got == (size_t)f + 1 && line[f] == '\n'); mh += got; read_ += (long)got; lines++; }
        }
        CHECK(r.len == mt - mh);
    }
    printf("written=%ld read=%ld discarded=%ld rejected_writes=%ld\n", written, read_, discarded, rejected);
    printf("lines=%ld zero_copy=%ld split_writes=%ld split_reads=%ld\n", lines, zero_copy, r.split_writes, r.split_reads);
    printf("buffered=%zu crc=%llu\n", r.len, crc);
    return 0;
}
