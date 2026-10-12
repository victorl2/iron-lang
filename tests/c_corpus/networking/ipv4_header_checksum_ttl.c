/*
 * title: IPv4 header build, checksum verification and incremental TTL update
 * topic: networking
 * covers: IPv4 header layout, ones complement checksum, RFC 1624 incremental update, header validation errors, single bit flip detection, IHL with options
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    unsigned tos, ttl, proto, id, flags, frag_off;
    uint32_t src, dst;
    size_t payload_len;
    const unsigned char *opts;
    size_t opt_len;
} Hdr;

static uint16_t csum(const unsigned char *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static size_t build(const Hdr *h, unsigned char *out) {
    size_t ihl = 5 + h->opt_len / 4;
    out[0] = (unsigned char)(0x40 | ihl);
    out[1] = (unsigned char)h->tos;
    unsigned tot = (unsigned)(ihl * 4 + h->payload_len);
    out[2] = (unsigned char)(tot >> 8); out[3] = (unsigned char)tot;
    out[4] = (unsigned char)(h->id >> 8); out[5] = (unsigned char)h->id;
    unsigned ff = (h->flags << 13) | h->frag_off;
    out[6] = (unsigned char)(ff >> 8); out[7] = (unsigned char)ff;
    out[8] = (unsigned char)h->ttl; out[9] = (unsigned char)h->proto;
    out[10] = out[11] = 0;
    for (int i = 0; i < 4; i++) { out[12 + i] = (unsigned char)(h->src >> (24 - 8 * i)); out[16 + i] = (unsigned char)(h->dst >> (24 - 8 * i)); }
    if (h->opt_len) memcpy(out + 20, h->opts, h->opt_len);
    uint16_t c = csum(out, ihl * 4);
    out[10] = (unsigned char)(c >> 8); out[11] = (unsigned char)c;
    return ihl * 4;
}

static const char *validate(const unsigned char *p, size_t avail) {
    if (avail < 20) return "truncated";
    if ((p[0] >> 4) != 4) return "bad-version";
    size_t ihl = (size_t)(p[0] & 15) * 4;
    if (ihl < 20) return "bad-ihl";
    if (avail < ihl) return "truncated";
    unsigned tot = (unsigned)((p[2] << 8) | p[3]);
    if (tot < ihl) return "bad-total-length";
    if (tot > avail) return "short-packet";
    if (csum(p, ihl) != 0) return "bad-checksum";
    return "ok";
}

static void hex(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x%s", p[i], (i % 4 == 3 && i + 1 < n) ? " " : "");
    printf("\n");
}

int main(void) {
    unsigned char pkt[128];
    memset(pkt, 0, sizeof pkt);
    Hdr h = { 0, 64, 17, 0x1c46, 2, 0, 0xc0a80001u, 0xc0a800c7u, 60, NULL, 0 };
    size_t n = build(&h, pkt);
    printf("header (%zu bytes): ", n);
    hex(pkt, n);
    printf("checksum field 0x%02x%02x, validate: %s\n", pkt[10], pkt[11], validate(pkt, n + 60 > sizeof pkt ? sizeof pkt : n + 60));
    CHECK(strcmp(validate(pkt, 80), "ok") == 0);

    /* Textbook example header (Wikipedia): 4500 0073 0000 4000 4011 b861 c0a8 0001 c0a8 00c7 */
    static const unsigned char ex[20] = { 0x45, 0, 0, 0x73, 0, 0, 0x40, 0, 0x40, 0x11, 0, 0, 0xc0, 0xa8, 0, 1, 0xc0, 0xa8, 0, 0xc7 };
    unsigned char e2[20];
    memcpy(e2, ex, 20);
    uint16_t c = csum(e2, 20);
    printf("known example checksum: 0x%04x\n", c);
    CHECK(c == 0xb861);

    /* Every single bit flip in the header must be detected. */
    int detected = 0, total = 0;
    for (size_t b = 0; b < n * 8; b++) {
        unsigned char t[64];
        memcpy(t, pkt, n);
        t[b / 8] ^= (unsigned char)(1u << (b % 8));
        total++;
        if (csum(t, n) != 0) detected++;
    }
    printf("single bit flips detected: %d of %d\n", detected, total);
    CHECK(detected == total);

    /* Router forwarding: decrement TTL and update checksum incrementally (RFC 1624 eqn 3). */
    int mism = 0, steps = 0;
    for (unsigned ttl = 255; ttl >= 1; ttl--) {
        Hdr q = h;
        q.ttl = ttl;
        q.id = (unsigned)(ttl * 257u);
        unsigned char a[20];
        build(&q, a);
        unsigned old_w = (unsigned)((a[8] << 8) | a[9]);
        a[8]--;
        unsigned new_w = (unsigned)((a[8] << 8) | a[9]);
        uint32_t hc = (uint32_t)((a[10] << 8) | a[11]);
        uint32_t sum = (~hc & 0xffffu) + (~old_w & 0xffffu) + new_w;
        sum = (sum & 0xffff) + (sum >> 16);
        sum = (sum & 0xffff) + (sum >> 16);
        uint16_t inc = (uint16_t)~sum;
        unsigned char full[20];
        memcpy(full, a, 20);
        full[10] = full[11] = 0;
        uint16_t fc = csum(full, 20);
        /* 0x0000 and 0xffff are both "zero" in ones complement */
        if (inc != fc && !((inc == 0 && fc == 0xffff) || (inc == 0xffff && fc == 0))) mism++;
        steps++;
    }
    printf("incremental TTL update vs full recompute: %d mismatches in %d steps\n", mism, steps);
    CHECK(mism == 0);

    /* Options: record route padded to 8 bytes gives IHL 7. */
    static const unsigned char opt[8] = { 0x07, 0x07, 0x04, 0, 0, 0, 0, 0 };
    Hdr o = h;
    o.opts = opt;
    o.opt_len = 8;
    o.payload_len = 12;
    size_t on = build(&o, pkt);
    printf("with options: ihl=%u header=%zu total=%u %s\n", pkt[0] & 15, on, (unsigned)((pkt[2] << 8) | pkt[3]), validate(pkt, on + 12));

    /* Validation error classes. */
    unsigned char t[64];
    build(&h, t);
    printf("errors:");
    printf(" %s", validate(t, 10));
    t[0] = 0x65; printf(" %s", validate(t, 80)); t[0] = 0x43; printf(" %s", validate(t, 80));
    build(&h, t); printf(" %s", validate(t, 40));
    t[3] = 10; printf(" %s", validate(t, 80));
    build(&h, t); t[16] ^= 1; printf(" %s\n", validate(t, 80));
    return 0;
}
