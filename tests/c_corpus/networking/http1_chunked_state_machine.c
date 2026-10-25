/*
 * title: HTTP/1.1 chunked transfer decoding as an incremental state machine
 * topic: networking
 * covers: chunked coding, resumable byte-at-a-time state machine, chunk extensions, trailer fields, size overflow rejection, pipelined leftovers, random split-point feeding
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef enum { S_SIZE, S_EXT, S_SIZE_LF, S_DATA, S_DATA_CR, S_DATA_LF, S_TR_START, S_TR_LINE, S_TR_LF, S_FINAL_LF, S_DONE, S_ERROR } State;
static const char *sname[] = { "size", "ext", "size-lf", "data", "data-cr", "data-lf", "trailer-start", "trailer-line", "trailer-lf", "final-lf", "done", "error" };

typedef struct {
    State st;
    uint32_t size, remaining;
    int digits;
    unsigned char *body; size_t blen, bcap;
    int chunks;
    char trailers[4][64]; int ntr; char cur[64]; size_t curlen;
    const char *err;
} Dec;

static void dec_init(Dec *d, unsigned char *body, size_t cap) {
    memset(d, 0, sizeof *d);
    d->st = S_SIZE;
    d->body = body;
    d->bcap = cap;
}

static int fail(Dec *d, const char *why) { d->st = S_ERROR; d->err = why; return 0; }

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Returns number of bytes consumed (stops at message end or error). */
static size_t dec_feed(Dec *d, const unsigned char *p, size_t n) {
    size_t i = 0;
    while (i < n && d->st != S_DONE && d->st != S_ERROR) {
        unsigned char c = p[i];
        switch (d->st) {
        case S_SIZE: {
            int h = hexv(c);
            if (h >= 0) {
                if (d->size > 0x07ffffffu) { fail(d, "chunk-size-overflow"); break; }
                d->size = d->size * 16 + (uint32_t)h;
                d->digits++;
                i++;
            } else if (d->digits == 0) {
                fail(d, "missing-chunk-size");
            } else if (c == ';') { d->st = S_EXT; i++; }
            else if (c == '\r') { d->st = S_SIZE_LF; i++; }
            else fail(d, "bad-chunk-size-char");
            break;
        }
        case S_EXT:
            if (c == '\r') d->st = S_SIZE_LF;
            else if (c == '\n' || c < 32) { fail(d, "bad-extension"); break; }
            i++;
            break;
        case S_SIZE_LF:
            if (c != '\n') { fail(d, "expected-LF"); break; }
            i++;
            d->chunks++;
            if (d->size == 0) d->st = S_TR_START;
            else { d->remaining = d->size; d->st = S_DATA; }
            break;
        case S_DATA: {
            size_t take = n - i < d->remaining ? n - i : d->remaining;
            if (d->blen + take > d->bcap) { fail(d, "body-too-large"); break; }
            memcpy(d->body + d->blen, p + i, take);
            d->blen += take;
            d->remaining -= (uint32_t)take;
            i += take;
            if (d->remaining == 0) d->st = S_DATA_CR;
            break;
        }
        case S_DATA_CR:
            if (c != '\r') { fail(d, "missing-CRLF-after-data"); break; }
            d->st = S_DATA_LF; i++;
            break;
        case S_DATA_LF:
            if (c != '\n') { fail(d, "missing-CRLF-after-data"); break; }
            d->st = S_SIZE; d->size = 0; d->digits = 0; i++;
            break;
        case S_TR_START:
            if (c == '\r') { d->st = S_FINAL_LF; i++; break; }
            d->curlen = 0;
            d->st = S_TR_LINE;
            break;
        case S_TR_LINE:
            if (c == '\r') { d->st = S_TR_LF; i++; break; }
            if (c == '\n' || d->curlen + 1 >= sizeof d->cur) { fail(d, "bad-trailer"); break; }
            d->cur[d->curlen++] = (char)c;
            i++;
            break;
        case S_TR_LF:
            if (c != '\n') { fail(d, "expected-LF"); break; }
            d->cur[d->curlen] = 0;
            if (!strchr(d->cur, ':')) { fail(d, "bad-trailer"); break; }
            if (d->ntr < 4) { snprintf(d->trailers[d->ntr], 64, "%s", d->cur); d->ntr++; }
            d->st = S_TR_START; i++;
            break;
        case S_FINAL_LF:
            if (c != '\n') { fail(d, "expected-LF"); break; }
            d->st = S_DONE; i++;
            break;
        default: break;
        }
    }
    return i;
}

static uint32_t rs = 0xc4a4c4a4u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

typedef struct { unsigned char *b; size_t n; } Out;
static void put(Out *o, const void *d, size_t n) { memcpy(o->b + o->n, d, n); o->n += n; }

static void encode_chunked(const unsigned char *body, size_t n, Out *o, int with_ext, int ntrail) {
    size_t pos = 0;
    char tmp[64];
    while (pos < n) {
        size_t c = 1 + rnd() % 300;
        if (c > n - pos) c = n - pos;
        int l = snprintf(tmp, sizeof tmp, (rnd() & 1) ? "%zx" : "%zX", c);
        put(o, tmp, (size_t)l);
        if (with_ext && (rnd() & 1)) { l = snprintf(tmp, sizeof tmp, ";name=v%u", (unsigned)(rnd() % 100)); put(o, tmp, (size_t)l); }
        put(o, "\r\n", 2);
        put(o, body + pos, c);
        put(o, "\r\n", 2);
        pos += c;
    }
    put(o, "0\r\n", 3);
    for (int i = 0; i < ntrail; i++) { int l = snprintf(tmp, sizeof tmp, "X-Trailer-%d: value-%d\r\n", i, i * 11); put(o, tmp, (size_t)l); }
    put(o, "\r\n", 2);
}

int main(void) {
    static unsigned char body[4096], wire[8192], got[4096];
    /* Worked example. */
    const char *ex = "4\r\nWiki\r\n6;lang=en\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\nExpires: never\r\nX-Sum: 42\r\n\r\nGET /next HTTP/1.1\r\n";
    Dec d;
    dec_init(&d, got, sizeof got);
    size_t used = dec_feed(&d, (const unsigned char *)ex, strlen(ex));
    printf("example: state=%s chunks=%d body=%zu bytes starting [%.9s] trailers=%d leftover=%zu bytes [%.10s]\n", sname[d.st], d.chunks - 1, d.blen, (const char *)got, d.ntr, strlen(ex + used), ex + used);
    for (int i = 0; i < d.ntr; i++) printf("  trailer: %s\n", d.trailers[i]);
    CHECK(d.st == S_DONE && d.blen == 24 && memcmp(got, "Wikipedia in \r\n\r\nchunks.", 24) == 0 && strcmp(ex + used, "GET /next HTTP/1.1\r\n") == 0);

    /* Random bodies, random chunking and random feed boundaries. */
    long wire_total = 0, body_total = 0;
    int max_chunks = 0;
    for (int t = 0; t < 300; t++) {
        size_t n = rnd() % 2000;
        for (size_t i = 0; i < n; i++) body[i] = (unsigned char)rnd();
        Out o = { wire, 0 };
        encode_chunked(body, n, &o, t % 2, (int)(rnd() % 4));
        for (int mode = 0; mode < 3; mode++) {
            dec_init(&d, got, sizeof got);
            size_t pos = 0;
            while (pos < o.n && d.st != S_DONE) {
                size_t step = mode == 0 ? o.n : (mode == 1 ? 1 : 1 + rnd() % 17);
                if (step > o.n - pos) step = o.n - pos;
                size_t used2 = dec_feed(&d, wire + pos, step);
                CHECK(d.st != S_ERROR);
                pos += used2;
            }
            CHECK(d.st == S_DONE && pos == o.n);
            CHECK(d.blen == n && memcmp(got, body, n) == 0);
        }
        wire_total += (long)o.n;
        body_total += (long)n;
        if (d.chunks - 1 > max_chunks) max_chunks = d.chunks - 1;
    }
    printf("300 random messages x 3 feed strategies ok: %ld wire bytes carried %ld body bytes, up to %d chunks\n", wire_total, body_total, max_chunks);

    /* Malformed inputs. */
    static const char *bad[] = {
        "g\r\nxx\r\n0\r\n\r\n", "\r\n", "3\r\nabcX\r\n0\r\n\r\n", "3\nabc\r\n0\r\n\r\n", "3\r\nabc\r\n0\r\nbad trailer\r\n\r\n",
        "fffffffff\r\n", "5;ext\nx\r\n", "-1\r\n", "3 \r\nabc\r\n", "3\r\nabc\r\n0\r\n\rX",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        dec_init(&d, got, sizeof got);
        dec_feed(&d, (const unsigned char *)bad[i], strlen(bad[i]));
        printf("bad %zu: %s (%s)\n", i + 1, sname[d.st], d.err ? d.err : "-");
        CHECK(d.st == S_ERROR);
    }
    /* Truncated stream stays resumable. */
    dec_init(&d, got, sizeof got);
    dec_feed(&d, (const unsigned char *)"5\r\nhel", 6);
    printf("truncated mid-chunk: state=%s remaining=%u so far [%.*s]\n", sname[d.st], (unsigned)d.remaining, (int)d.blen, (const char *)got);
    dec_feed(&d, (const unsigned char *)"lo\r\n0\r\n\r\n", 9);
    CHECK(d.st == S_DONE && d.blen == 5);
    printf("resumed: state=%s body=[%.*s]\n", sname[d.st], (int)d.blen, (const char *)got);
    return 0;
}
