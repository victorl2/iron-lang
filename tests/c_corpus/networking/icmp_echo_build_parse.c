/*
 * title: ICMP echo, echo reply and destination unreachable packets
 * topic: networking
 * covers: ICMP header layout, checksum, echo request to reply transformation, quoted original datagram in errors, parse validation, sequence tracking
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { T_ECHO_REPLY = 0, T_DEST_UNREACH = 3, T_ECHO = 8, T_TIME_EXCEEDED = 11 };

static uint16_t csum(const unsigned char *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static size_t build_echo(unsigned char *o, int type, unsigned id, unsigned seq, const unsigned char *data, size_t n) {
    o[0] = (unsigned char)type; o[1] = 0; o[2] = o[3] = 0;
    o[4] = (unsigned char)(id >> 8); o[5] = (unsigned char)id; o[6] = (unsigned char)(seq >> 8); o[7] = (unsigned char)seq;
    memcpy(o + 8, data, n);
    uint16_t c = csum(o, 8 + n);
    o[2] = (unsigned char)(c >> 8); o[3] = (unsigned char)c;
    return 8 + n;
}

typedef struct { int type, code; unsigned id, seq; size_t data_len; const unsigned char *data; } Icmp;

static const char *parse(const unsigned char *p, size_t n, Icmp *out) {
    if (n < 8) return "truncated";
    if (csum(p, n) != 0) return "bad-checksum";
    out->type = p[0]; out->code = p[1];
    out->id = (unsigned)((p[4] << 8) | p[5]);
    out->seq = (unsigned)((p[6] << 8) | p[7]);
    out->data = p + 8; out->data_len = n - 8;
    if ((p[0] == T_ECHO || p[0] == T_ECHO_REPLY) && p[1] != 0) return "bad-code";
    return "ok";
}

/* Turn a request into a reply in place using an incremental checksum update (type changes 8 -> 0). */
static void make_reply(unsigned char *p, size_t n) {
    (void)n;
    unsigned old_w = (unsigned)((p[0] << 8) | p[1]);
    p[0] = T_ECHO_REPLY;
    unsigned new_w = (unsigned)((p[0] << 8) | p[1]);
    uint32_t hc = (uint32_t)((p[2] << 8) | p[3]);
    uint32_t s = (~hc & 0xffffu) + (~old_w & 0xffffu) + new_w;
    s = (s & 0xffff) + (s >> 16);
    s = (s & 0xffff) + (s >> 16);
    uint16_t c = (uint16_t)~s;
    p[2] = (unsigned char)(c >> 8); p[3] = (unsigned char)c;
}

static size_t build_error(unsigned char *o, int type, int code, const unsigned char *orig_dgram, size_t orig_n, unsigned rest) {
    size_t quote = orig_n < 28 ? orig_n : 28; /* IP header + 8 bytes */
    o[0] = (unsigned char)type; o[1] = (unsigned char)code; o[2] = o[3] = 0;
    o[4] = (unsigned char)(rest >> 24); o[5] = (unsigned char)(rest >> 16); o[6] = (unsigned char)(rest >> 8); o[7] = (unsigned char)rest;
    memcpy(o + 8, orig_dgram, quote);
    uint16_t c = csum(o, 8 + quote);
    o[2] = (unsigned char)(c >> 8); o[3] = (unsigned char)c;
    return 8 + quote;
}

static const char *tname(int t) {
    switch (t) {
    case T_ECHO_REPLY: return "echo-reply";
    case T_DEST_UNREACH: return "dest-unreachable";
    case T_ECHO: return "echo-request";
    case T_TIME_EXCEEDED: return "time-exceeded";
    default: return "other";
    }
}

int main(void) {
    unsigned char req[128], rep[128];
    unsigned char data[56];
    /* Classic ping payload: incrementing bytes. */
    for (size_t i = 0; i < sizeof data; i++) data[i] = (unsigned char)(0x10 + i);
    int lost = 0, ok = 0;
    for (unsigned seq = 1; seq <= 6; seq++) {
        size_t n = build_echo(req, T_ECHO, 0xbeef, seq, data, sizeof data);
        if (seq == 4) { /* simulate corruption on the reply path */
            memcpy(rep, req, n);
            make_reply(rep, n);
            rep[20] ^= 0x40;
            Icmp r;
            const char *st = parse(rep, n, &r);
            printf("seq %u: reply %s\n", seq, st);
            CHECK(strcmp(st, "bad-checksum") == 0);
            lost++;
            continue;
        }
        Icmp q, r;
        CHECK(strcmp(parse(req, n, &q), "ok") == 0);
        memcpy(rep, req, n);
        make_reply(rep, n);
        /* the incrementally updated reply must equal a freshly built one */
        unsigned char fresh[128];
        size_t fn = build_echo(fresh, T_ECHO_REPLY, q.id, q.seq, q.data, q.data_len);
        CHECK(fn == n && memcmp(fresh, rep, n) == 0);
        CHECK(strcmp(parse(rep, n, &r), "ok") == 0);
        CHECK(r.id == 0xbeef && r.seq == seq && r.data_len == sizeof data && memcmp(r.data, data, sizeof data) == 0);
        printf("seq %u: %s id=0x%04x len=%zu checksum=0x%02x%02x\n", seq, tname(r.type), r.id, n, rep[2], rep[3]);
        ok++;
    }
    printf("%d replies, %d lost\n", ok, lost);

    /* Different payload sizes including odd. */
    for (size_t pn = 0; pn <= 9; pn += 3) {
        size_t n = build_echo(req, T_ECHO, 1, 1, data, pn);
        Icmp q;
        CHECK(strcmp(parse(req, n, &q), "ok") == 0 && q.data_len == pn);
        printf("payload %zu -> packet %zu bytes, checksum 0x%02x%02x\n", pn, n, req[2], req[3]);
    }

    /* Errors quoting a UDP datagram to closed port and a TTL expiry. */
    unsigned char orig[40] = { 0x45, 0, 0, 40, 0x12, 0x34, 0x40, 0, 1, 17, 0, 0, 10, 0, 0, 5, 10, 0, 0, 9, 0x9c, 0x40, 0x00, 0x35, 0, 20, 0, 0 };
    unsigned char err[64];
    size_t en = build_error(err, T_DEST_UNREACH, 3, orig, sizeof orig, 0);
    Icmp e;
    const char *pst = parse(err, en, &e);
    printf("%s code=%d quoted=%zu bytes, quoted proto=%u dst port=%u: %s\n", tname(err[0]), err[1], e.data_len, err[8 + 9], (unsigned)((err[8 + 22] << 8) | err[8 + 23]), pst);
    en = build_error(err, T_TIME_EXCEEDED, 0, orig, sizeof orig, 0);
    CHECK(strcmp(parse(err, en, &e), "ok") == 0 && e.type == T_TIME_EXCEEDED && e.data_len == 28);
    printf("%s quoted %zu bytes ttl=%u\n", tname(e.type), e.data_len, e.data[8]);

    /* Parse rejections. */
    Icmp tmp;
    size_t n = build_echo(req, T_ECHO, 7, 7, data, 4);
    printf("rejections: %s", parse(req, 5, &tmp));
    req[1] = 3; printf(" %s", parse(req, n, &tmp));
    n = build_echo(req, T_ECHO, 7, 7, data, 4); req[1] = 3;
    uint16_t c = 0; req[2] = req[3] = 0; c = csum(req, n); req[2] = (unsigned char)(c >> 8); req[3] = (unsigned char)c;
    printf(" %s\n", parse(req, n, &tmp));
    return 0;
}
