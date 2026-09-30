/*
 * title: WebSocket frame stream parser with protocol validation
 * topic: networking
 * covers: RFC 6455 frame layout, masking rules per side, minimal length encodings, fragmentation state, interleaved control frames, incremental UTF-8 validation across fragments, close code validation, RFC 5.7 example frames
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { unsigned rem, lo, hi; } Utf8;
static void u8_init(Utf8 *u) { u->rem = 0; u->lo = 0x80; u->hi = 0xbf; }
static int u8_feed(Utf8 *u, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned b = p[i];
        if (u->rem == 0) {
            if (b < 0x80) continue;
            u->lo = 0x80; u->hi = 0xbf;
            if (b >= 0xc2 && b <= 0xdf) u->rem = 1;
            else if (b == 0xe0) { u->rem = 2; u->lo = 0xa0; }
            else if (b == 0xed) { u->rem = 2; u->hi = 0x9f; }
            else if (b >= 0xe1 && b <= 0xef) u->rem = 2;
            else if (b == 0xf0) { u->rem = 3; u->lo = 0x90; }
            else if (b == 0xf4) { u->rem = 3; u->hi = 0x8f; }
            else if (b >= 0xf1 && b <= 0xf3) u->rem = 3;
            else return 0;
        } else {
            if (b < u->lo || b > u->hi) return 0;
            u->rem--;
            u->lo = 0x80; u->hi = 0xbf;
        }
    }
    return 1;
}

typedef struct { int fin, rsv, op, masked; uint64_t len; unsigned char key[4]; size_t hdr; } Hdr;

/* 1 = complete header parsed, 0 = need more, -1 = protocol error (reason via *why) */
static int parse_header(const unsigned char *p, size_t n, Hdr *h, const char **why) {
    if (n < 2) return 0;
    h->fin = p[0] >> 7; h->rsv = (p[0] >> 4) & 7; h->op = p[0] & 15; h->masked = p[1] >> 7;
    uint64_t l = p[1] & 127;
    size_t o = 2;
    if (h->rsv) { *why = "reserved-bits-set"; return -1; }
    if ((h->op >= 3 && h->op <= 7) || h->op >= 11) { *why = "reserved-opcode"; return -1; }
    if (h->op >= 8 && (!h->fin || l > 125)) { *why = "bad-control-frame"; return -1; }
    if (l == 126) {
        if (n < 4) return 0;
        l = ((uint64_t)p[2] << 8) | p[3];
        if (l < 126) { *why = "non-minimal-length"; return -1; }
        o = 4;
    } else if (l == 127) {
        if (n < 10) return 0;
        l = 0;
        for (int i = 0; i < 8; i++) l = (l << 8) | p[2 + i];
        if (l >> 63) { *why = "length-msb-set"; return -1; }
        if (l < 65536) { *why = "non-minimal-length"; return -1; }
        o = 10;
    }
    if (h->masked) {
        if (n < o + 4) return 0;
        memcpy(h->key, p + o, 4);
        o += 4;
    }
    h->len = l;
    h->hdr = o;
    return 1;
}

typedef struct {
    int server_side; /* parsing frames from a client: must be masked */
    unsigned char buf[80000]; size_t have;
    int in_msg, msg_op;
    Utf8 u8;
    size_t max_msg, msg_len;
    unsigned char msg[70000];
    int closed;
    const char *err;
    int events, quiet;
} Conn;

static int close_code_ok(unsigned c) {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

static int protocol_fail(Conn *c, const char *why) { c->err = why; c->closed = 1; return 0; }

static void event(Conn *c, const char *fmt, const unsigned char *d, size_t n, unsigned code) {
    c->events++;
    if (c->quiet) return;
    printf("    %s", fmt);
    if (code) printf(" code=%u", code);
    if (d && n) {
        if (n <= 12) printf(" [%.*s]", (int)n, (const char *)d); else printf(" (%zu bytes)", n);
    }
    printf("\n");
}

static void feed(Conn *c, const unsigned char *data, size_t n) {
    CHECK(c->have + n <= sizeof c->buf);
    memcpy(c->buf + c->have, data, n);
    c->have += n;
    size_t pos = 0;
    while (!c->closed) {
        Hdr h;
        const char *why = NULL;
        int r = parse_header(c->buf + pos, c->have - pos, &h, &why);
        if (r < 0) { protocol_fail(c, why); break; }
        if (r == 0) break;
        if (c->server_side && !h.masked) { protocol_fail(c, "unmasked-client-frame"); break; }
        if (!c->server_side && h.masked) { protocol_fail(c, "masked-server-frame"); break; }
        if (h.len > c->max_msg) { protocol_fail(c, "message-too-big"); break; }
        if (c->have - pos < h.hdr + h.len) break;
        unsigned char pl[70000];
        memcpy(pl, c->buf + pos + h.hdr, (size_t)h.len);
        if (h.masked) for (size_t i = 0; i < h.len; i++) pl[i] ^= h.key[i & 3];
        pos += h.hdr + (size_t)h.len;
        size_t len = (size_t)h.len;
        if (h.op >= 8) {
            if (h.op == 8) {
                unsigned code = 0;
                if (len == 1) { protocol_fail(c, "close-payload-1-byte"); break; }
                if (len >= 2) {
                    code = (unsigned)((pl[0] << 8) | pl[1]);
                    if (!close_code_ok(code)) { protocol_fail(c, "bad-close-code"); break; }
                    Utf8 u; u8_init(&u);
                    if (!u8_feed(&u, pl + 2, len - 2) || u.rem) { protocol_fail(c, "close-reason-not-utf8"); break; }
                }
                event(c, "CLOSE", len > 2 ? pl + 2 : NULL, len > 2 ? len - 2 : 0, code);
                c->closed = 1;
            } else {
                event(c, h.op == 9 ? "PING" : "PONG", pl, len, 0);
            }
            continue;
        }
        if (h.op == 0) {
            if (!c->in_msg) { protocol_fail(c, "unexpected-continuation"); break; }
        } else {
            if (c->in_msg) { protocol_fail(c, "new-data-frame-inside-fragmented-message"); break; }
            c->in_msg = 1; c->msg_op = h.op; c->msg_len = 0;
            u8_init(&c->u8);
        }
        if (c->msg_len + len > c->max_msg) { protocol_fail(c, "message-too-big"); break; }
        if (c->msg_op == 1 && !u8_feed(&c->u8, pl, len)) { protocol_fail(c, "invalid-utf8"); break; }
        memcpy(c->msg + c->msg_len, pl, len);
        c->msg_len += len;
        if (h.fin) {
            if (c->msg_op == 1 && c->u8.rem) { protocol_fail(c, "truncated-utf8"); break; }
            event(c, c->msg_op == 1 ? "TEXT" : "BINARY", c->msg, c->msg_len, 0);
            c->in_msg = 0;
        }
    }
    memmove(c->buf, c->buf + pos, c->have - pos);
    c->have -= pos;
}

static size_t encode(unsigned char *o, int fin, int op, const unsigned char *pl, size_t n, const unsigned char *key) {
    size_t k = 0;
    o[k++] = (unsigned char)((fin ? 0x80 : 0) | op);
    unsigned m = key ? 0x80 : 0;
    if (n < 126) o[k++] = (unsigned char)(m | n);
    else if (n < 65536) { o[k++] = (unsigned char)(m | 126); o[k++] = (unsigned char)(n >> 8); o[k++] = (unsigned char)n; }
    else { o[k++] = (unsigned char)(m | 127); for (int i = 7; i >= 0; i--) o[k++] = (unsigned char)((uint64_t)n >> (8 * i)); }
    if (key) { memcpy(o + k, key, 4); k += 4; }
    for (size_t i = 0; i < n; i++) o[k + i] = key ? (unsigned char)(pl[i] ^ key[i & 3]) : pl[i];
    return k + n;
}

static Conn *newconn(int server_side) {
    Conn *c = calloc(1, sizeof *c);
    CHECK(c);
    c->server_side = server_side;
    c->max_msg = 69000;
    return c;
}

static uint32_t rs = 0x77656273u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    /* RFC 6455 section 5.7 examples. */
    struct { const char *label; int server; size_t n; unsigned char b[16]; } ex[] = {
        { "unmasked text Hello (server to client)", 0, 7, { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f } },
        { "masked text Hello (client to server)", 1, 11, { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 } },
        { "fragmented Hel + lo", 0, 9, { 0x01, 0x03, 0x48, 0x65, 0x6c, 0x80, 0x02, 0x6c, 0x6f } },
        { "unmasked ping Hello", 0, 7, { 0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f } },
        { "masked pong Hello", 1, 11, { 0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 } },
    };
    for (size_t i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        Conn *c = newconn(ex[i].server);
        printf("%s\n", ex[i].label);
        feed(c, ex[i].b, ex[i].n);
        CHECK(!c->closed && c->events == 1 && c->have == 0);
        free(c);
    }
    /* 256 and 65536 byte binary messages use the 16 and 64 bit length forms. */
    static unsigned char big[70000], wire[70100];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (unsigned char)(i * 31);
    size_t sizes[] = { 125, 126, 256, 65535, 65536, 69000 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t n = encode(wire, 1, 2, big, sizes[i], NULL);
        size_t hdr = n - sizes[i];
        Conn *c = newconn(0);
        feed(c, wire, n);
        CHECK(!c->closed && c->events == 1 && c->msg_len == sizes[i] && memcmp(c->msg, big, sizes[i]) == 0);
        printf("payload %5zu -> header %zu bytes\n", sizes[i], hdr);
        free(c);
    }
    /* Protocol violations. */
    struct { const char *label; int server; size_t n; unsigned char b[24]; } bad[] = {
        { "unmasked frame from client", 1, 7, { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f } },
        { "masked frame from server", 0, 11, { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 } },
        { "RSV1 set", 0, 2, { 0xc1, 0x00 } },
        { "reserved opcode 3", 0, 2, { 0x83, 0x00 } },
        { "fragmented ping", 0, 2, { 0x09, 0x00 } },
        { "ping with 126 byte length", 0, 4, { 0x89, 0x7e, 0x00, 0x7e } },
        { "non-minimal 16-bit length", 0, 4, { 0x82, 0x7e, 0x00, 0x05 } },
        { "continuation with nothing open", 0, 3, { 0x80, 0x01, 0x41 } },
        { "data frame inside fragmented msg", 0, 6, { 0x01, 0x01, 0x41, 0x81, 0x01, 0x42 } },
        { "invalid UTF-8 byte", 0, 4, { 0x81, 0x02, 0xc0, 0xaf } },
        { "euro sign split across fragments", 0, 7, { 0x01, 0x02, 0xe2, 0x82, 0x80, 0x01, 0xac } },
        { "lead byte then ASCII in next fragment", 0, 6, { 0x01, 0x01, 0xe2, 0x80, 0x01, 0x28 } },
        { "truncated UTF-8 at fin", 0, 4, { 0x81, 0x02, 0xe2, 0x82 } },
        { "surrogate encoded in UTF-8", 0, 5, { 0x81, 0x03, 0xed, 0xa0, 0x80 } },
        { "close with 1 byte payload", 0, 3, { 0x88, 0x01, 0x03 } },
        { "close code 1005 on wire", 0, 4, { 0x88, 0x02, 0x03, 0xed } },
        { "close code 1000 with reason", 0, 8, { 0x88, 0x06, 0x03, 0xe8, 'b', 'y', 'e', '!' } },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Conn *c = newconn(bad[i].server);
        printf("%s\n", bad[i].label);
        feed(c, bad[i].b, bad[i].n);
        if (c->err) printf("    -> protocol error: %s\n", c->err);
        else printf("    -> no error, closed=%d pending bytes=%zu\n", c->closed, c->have);
        free(c);
    }
    /* Randomized: fragmented messages with interleaved pings, masked, fed in random chunk sizes. */
    long msgs = 0, pings = 0, bytes = 0;
    for (int t = 0; t < 60; t++) {
        long pings0 = pings;
        Conn *c = newconn(1);
        static unsigned char stream[200000];
        size_t sn = 0;
        int nm = 1 + (int)(rnd() % 4);
        int want_op[4];
        static unsigned char payloads[4][20000];
        for (int m = 0; m < nm; m++) {
            int text = (int)(rnd() & 1);
            size_t len = rnd() % 20000;
            for (size_t i = 0; i < len; i++) payloads[m][i] = text ? (unsigned char)('a' + rnd() % 26) : (unsigned char)rnd();
            want_op[m] = text ? 1 : 2;
            size_t pos = 0;
            int first = 1;
            do {
                size_t piece = len - pos;
                if (piece > 1) piece = 1 + rnd() % piece;
                unsigned char key[4];
                uint32_t kk = rnd();
                memcpy(key, &kk, 4);
                int fin = (pos + piece == len);
                sn += encode(stream + sn, fin, first ? want_op[m] : 0, payloads[m] + pos, piece, key);
                pos += piece;
                first = 0;
                if (!fin && (rnd() % 3 == 0)) { sn += encode(stream + sn, 1, 9, (const unsigned char *)"hb", 2, key); pings++; }
            } while (pos < len);
            msgs++;
        }
        size_t pos = 0;
        c->quiet = 1;
        while (pos < sn) {
            size_t step = 1 + rnd() % 3000;
            if (step > sn - pos) step = sn - pos;
            feed(c, stream + pos, step);
            CHECK(!c->closed);
            pos += step;
        }
        CHECK(c->have == 0 && c->events == nm + (int)(pings - pings0));
        bytes += (long)sn;
        free(c);
    }
    printf("random streams: %ld messages, %ld interleaved pings, %ld wire bytes decoded\n", msgs, pings, bytes);
    return 0;
}
