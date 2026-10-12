/*
 * title: UDP and TCP segments with IPv4 and IPv6 pseudo-header checksums
 * topic: networking
 * covers: transport checksum, pseudo header construction, odd length padding, UDP zero checksum rule, TCP flags, two independent checksum implementations
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Implementation A: 32-bit accumulator with late folding. */
static uint16_t sum_a(const unsigned char *p, size_t n, uint32_t init) {
    uint32_t s = init;
    size_t i = 0;
    for (; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (i < n) s += (uint32_t)(p[i] << 8);
    while (s >> 16) s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)s;
}

/* Implementation B: 16-bit ones complement addition with end-around carry after every word. */
static uint16_t add1c(uint16_t a, uint16_t b) {
    uint32_t s = (uint32_t)a + b;
    return (uint16_t)((s & 0xffff) + (s >> 16));
}
static uint16_t sum_b(const unsigned char *p, size_t n) {
    uint16_t acc = 0;
    for (size_t i = 0; i < n; i += 2) {
        uint16_t w = (uint16_t)(p[i] << 8);
        if (i + 1 < n) w = (uint16_t)(w | p[i + 1]);
        acc = add1c(acc, w);
    }
    return acc;
}

static size_t pseudo4(unsigned char *o, uint32_t src, uint32_t dst, unsigned proto, size_t len) {
    for (int i = 0; i < 4; i++) { o[i] = (unsigned char)(src >> (24 - 8 * i)); o[4 + i] = (unsigned char)(dst >> (24 - 8 * i)); }
    o[8] = 0; o[9] = (unsigned char)proto; o[10] = (unsigned char)(len >> 8); o[11] = (unsigned char)len;
    return 12;
}
static size_t pseudo6(unsigned char *o, const unsigned char *src, const unsigned char *dst, unsigned proto, size_t len) {
    memcpy(o, src, 16); memcpy(o + 16, dst, 16);
    o[32] = (unsigned char)(len >> 24); o[33] = (unsigned char)(len >> 16); o[34] = (unsigned char)(len >> 8); o[35] = (unsigned char)len;
    o[36] = o[37] = o[38] = 0; o[39] = (unsigned char)proto;
    return 40;
}

static uint16_t transport_csum(const unsigned char *ph, size_t phn, const unsigned char *seg, size_t n) {
    unsigned char buf[2048];
    memcpy(buf, ph, phn);
    memcpy(buf + phn, seg, n);
    uint16_t a = (uint16_t)~sum_a(buf, phn + n, 0);
    uint16_t b = (uint16_t)~sum_b(buf, phn + n);
    CHECK(a == b);
    return a;
}

static size_t build_udp(unsigned char *o, unsigned sp, unsigned dp, const unsigned char *pay, size_t n) {
    o[0] = (unsigned char)(sp >> 8); o[1] = (unsigned char)sp; o[2] = (unsigned char)(dp >> 8); o[3] = (unsigned char)dp;
    o[4] = (unsigned char)((n + 8) >> 8); o[5] = (unsigned char)(n + 8); o[6] = o[7] = 0;
    memcpy(o + 8, pay, n);
    return n + 8;
}

static size_t build_tcp(unsigned char *o, unsigned sp, unsigned dp, uint32_t seq, uint32_t ack, unsigned flags, unsigned win, const unsigned char *pay, size_t n) {
    o[0] = (unsigned char)(sp >> 8); o[1] = (unsigned char)sp; o[2] = (unsigned char)(dp >> 8); o[3] = (unsigned char)dp;
    for (int i = 0; i < 4; i++) { o[4 + i] = (unsigned char)(seq >> (24 - 8 * i)); o[8 + i] = (unsigned char)(ack >> (24 - 8 * i)); }
    o[12] = 5 << 4; o[13] = (unsigned char)flags;
    o[14] = (unsigned char)(win >> 8); o[15] = (unsigned char)win; o[16] = o[17] = o[18] = o[19] = 0;
    memcpy(o + 20, pay, n);
    return n + 20;
}

static void flags_str(unsigned f, char *s) {
    static const char names[8] = { 'F', 'S', 'R', 'P', 'A', 'U', 'E', 'C' };
    int k = 0;
    for (int i = 7; i >= 0; i--) if (f & (1u << i)) s[k++] = names[i];
    if (!k) s[k++] = '.';
    s[k] = 0;
}

static uint32_t rs = 0x5eed5eedu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    unsigned char seg[1600], ph[64];
    uint32_t src = 0x0a000001u, dst = 0x0a000002u;

    /* UDP over IPv4, even and odd payload. */
    static const char *msgs[] = { "hello, udp", "odd length!", "" };
    for (int m = 0; m < 3; m++) {
        size_t pn = strlen(msgs[m]);
        size_t n = build_udp(seg, 5353, 53, (const unsigned char *)msgs[m], pn);
        size_t pl = pseudo4(ph, src, dst, 17, n);
        uint16_t c = transport_csum(ph, pl, seg, n);
        if (c == 0) c = 0xffff;
        seg[6] = (unsigned char)(c >> 8); seg[7] = (unsigned char)c;
        /* verify: sum over pseudo header + segment including checksum is 0xffff */
        unsigned char v[2048];
        memcpy(v, ph, pl); memcpy(v + pl, seg, n);
        CHECK(sum_a(v, pl + n, 0) == 0xffff);
        printf("udp v4 payload %-13s len=%2zu checksum=0x%04x\n", pn ? msgs[m] : "(empty)", n, c);
    }

    /* TCP handshake segments. */
    static const unsigned fl[] = { 0x02, 0x12, 0x10, 0x18, 0x11 };
    for (int i = 0; i < 5; i++) {
        const char *pay = (fl[i] & 8) ? "GET / HTTP/1.1\r\n\r\n" : "";
        size_t n = build_tcp(seg, 40000, 80, 1000u + (uint32_t)i * 100, 5000u, fl[i], 65535, (const unsigned char *)pay, strlen(pay));
        size_t pl = pseudo4(ph, src, dst, 6, n);
        uint16_t c = transport_csum(ph, pl, seg, n);
        seg[16] = (unsigned char)(c >> 8); seg[17] = (unsigned char)c;
        unsigned char v[2048];
        memcpy(v, ph, pl); memcpy(v + pl, seg, n);
        CHECK(sum_a(v, pl + n, 0) == 0xffff);
        char fs[10];
        flags_str(fl[i], fs);
        printf("tcp flags=%-3s seq=%u len=%2zu checksum=0x%04x\n", fs, 1000u + (unsigned)i * 100, n, c);
    }

    /* IPv6 pseudo header: UDP checksum is mandatory. */
    unsigned char s6[16] = { 0x20, 0x01, 0x0d, 0xb8 }, d6[16] = { 0x20, 0x01, 0x0d, 0xb8 };
    s6[15] = 1; d6[15] = 2;
    size_t n = build_udp(seg, 1234, 4321, (const unsigned char *)"v6 payload", 10);
    size_t pl = pseudo6(ph, s6, d6, 17, n);
    uint16_t c6 = transport_csum(ph, pl, seg, n);
    printf("udp v6 checksum=0x%04x\n", c6);

    /* Fuzz: random segments, both implementations agree and verification holds; corrupt one byte -> detected. */
    int detected = 0, undetected = 0;
    for (int t = 0; t < 500; t++) {
        size_t pn = rnd() % 200;
        unsigned char pay[200];
        for (size_t i = 0; i < pn; i++) pay[i] = (unsigned char)rnd();
        uint32_t a = rnd(), b = rnd();
        uint32_t r1 = rnd(), r2 = rnd(), r3 = rnd(), r4 = rnd(), r5 = rnd(), r6 = rnd();
        size_t sn = build_tcp(seg, r1 & 0xffff, r2 & 0xffff, r3, r4, r5 & 0xff, r6 & 0xffff, pay, pn);
        size_t pn4 = pseudo4(ph, a, b, 6, sn);
        uint16_t c = transport_csum(ph, pn4, seg, sn);
        seg[16] = (unsigned char)(c >> 8); seg[17] = (unsigned char)c;
        unsigned char v[2048];
        memcpy(v, ph, pn4); memcpy(v + pn4, seg, sn);
        CHECK(sum_a(v, pn4 + sn, 0) == 0xffff);
        size_t pos = pn4 + rnd() % sn;
        v[pos] ^= (unsigned char)(1u << (rnd() % 8));
        if (sum_a(v, pn4 + sn, 0) != 0xffff) detected++; else undetected++;
    }
    printf("fuzz: 500 segments verified, single-bit corruption detected %d, missed %d\n", detected, undetected);
    CHECK(undetected == 0);
    return 0;
}
