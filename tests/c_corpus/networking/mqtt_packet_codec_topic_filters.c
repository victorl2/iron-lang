/*
 * title: MQTT 3.1.1 packet codec, remaining length limits and topic filter matching
 * topic: networking
 * covers: remaining length varint boundaries, non-minimal and overlong rejection, CONNECT and PUBLISH and SUBSCRIBE encoding, fixed header flag validation, UTF-8 string prefixes, wildcard filter matching, dollar topics, stream splitting
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static size_t enc_rl(unsigned char *o, uint32_t v) {
    size_t n = 0;
    do {
        unsigned char b = (unsigned char)(v % 128);
        v /= 128;
        if (v) b |= 128;
        o[n++] = b;
    } while (v);
    return n;
}

/* 1 ok, 0 need more, -1 malformed (overlong), -2 non-minimal */
static int dec_rl(const unsigned char *p, size_t n, uint32_t *v, size_t *used) {
    uint32_t val = 0, mult = 1;
    for (size_t i = 0; i < 4; i++) {
        if (i >= n) return 0;
        val += (uint32_t)(p[i] & 127) * mult;
        if (!(p[i] & 128)) {
            if (i > 0 && p[i] == 0) return -2;
            *v = val; *used = i + 1;
            return 1;
        }
        mult *= 128;
    }
    return -1;
}

typedef struct { unsigned char b[1024]; size_t n; } Buf;
static void b8(Buf *b, unsigned v) { b->b[b->n++] = (unsigned char)v; }
static void b16(Buf *b, unsigned v) { b8(b, v >> 8); b8(b, v & 255); }
static void bstr(Buf *b, const char *s) { size_t l = strlen(s); b16(b, (unsigned)l); memcpy(b->b + b->n, s, l); b->n += l; }

static size_t wrap(unsigned char *out, unsigned type, unsigned flags, const Buf *body) {
    size_t k = 0;
    out[k++] = (unsigned char)((type << 4) | flags);
    k += enc_rl(out + k, (uint32_t)body->n);
    memcpy(out + k, body->b, body->n);
    return k + body->n;
}

static int topic_name_ok(const char *t) { return t[0] && !strpbrk(t, "+#"); }

static int filter_ok(const char *f) {
    if (!*f) return 0;
    for (const char *p = f; *p; p++) {
        int start = (p == f || p[-1] == '/');
        int end = (p[1] == 0 || p[1] == '/');
        if (*p == '+' && !(start && end)) return 0;
        if (*p == '#' && !(start && p[1] == 0)) return 0;
    }
    return 1;
}

static int match(const char *f, const char *t) {
    if (t[0] == '$' && (f[0] == '+' || f[0] == '#')) return 0;
    for (;;) {
        if (*f == '#') return 1;
        size_t fl = strcspn(f, "/"), tl = strcspn(t, "/");
        if (*f == '+' && fl == 1) { /* any single level */ }
        else if (fl != tl || strncmp(f, t, fl) != 0) return 0;
        f += fl; t += tl;
        if (*f == 0 && *t == 0) return 1;
        if (*f == '/' && f[1] == '#' && f[2] == 0 && *t == 0) return 1; /* parent level matches "a/#" */
        if (*f != '/' || *t != '/') return 0;
        f++; t++;
    }
}

typedef struct { unsigned type, flags; const unsigned char *body; size_t blen; } Pkt;

static const char *rd_str(const unsigned char **p, const unsigned char *end, char *out, size_t cap) {
    if (end - *p < 2) return "short-string-prefix";
    size_t l = (size_t)(((*p)[0] << 8) | (*p)[1]);
    *p += 2;
    if ((size_t)(end - *p) < l) return "short-string";
    if (l >= cap) return "string-too-long";
    memcpy(out, *p, l);
    out[l] = 0;
    *p += l;
    return NULL;
}

static const char *describe(const Pkt *pk) {
    static char msg[300];
    const unsigned char *p = pk->body, *end = pk->body + pk->blen;
    char s1[64], s2[64], s3[64];
    const char *e;
    static const char *names[] = { "?", "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL", "PUBCOMP", "SUBSCRIBE", "SUBACK", "UNSUBSCRIBE", "UNSUBACK", "PINGREQ", "PINGRESP", "DISCONNECT", "?" };
    switch (pk->type) {
    case 1: {
        if ((e = rd_str(&p, end, s1, sizeof s1))) return e;
        if (strcmp(s1, "MQTT") != 0) return "bad-protocol-name";
        if (end - p < 4) return "short-connect";
        unsigned lvl = p[0], fl = p[1], ka = (unsigned)((p[2] << 8) | p[3]);
        p += 4;
        if (lvl != 4) return "unsupported-level";
        if (fl & 1) return "reserved-connect-flag";
        if ((fl & 4) == 0 && (fl & 0x38)) return "will-flags-without-will";
        if ((fl & 0x80) == 0 && (fl & 0x40)) return "password-without-username";
        if ((e = rd_str(&p, end, s1, sizeof s1))) return e;
        char extra[120] = "";
        if (fl & 4) {
            if ((e = rd_str(&p, end, s2, sizeof s2)) || (e = rd_str(&p, end, s3, sizeof s3))) return e;
            snprintf(extra + strlen(extra), sizeof extra - strlen(extra), " will=%s:%s(qos%u%s)", s2, s3, (fl >> 3) & 3, (fl & 0x20) ? ",retain" : "");
        }
        if (fl & 0x80) { if ((e = rd_str(&p, end, s2, sizeof s2))) return e; snprintf(extra + strlen(extra), sizeof extra - strlen(extra), " user=%s", s2); }
        if (fl & 0x40) { if ((e = rd_str(&p, end, s2, sizeof s2))) return e; snprintf(extra + strlen(extra), sizeof extra - strlen(extra), " pass=%zu bytes", strlen(s2)); }
        if (p != end) return "trailing-bytes";
        snprintf(msg, sizeof msg, "CONNECT id=%s keepalive=%u clean=%u%s", s1, ka, (fl >> 1) & 1, extra);
        return msg;
    }
    case 3: {
        unsigned qos = (pk->flags >> 1) & 3;
        if (qos == 3) return "invalid-qos-3";
        if ((e = rd_str(&p, end, s1, sizeof s1))) return e;
        if (!topic_name_ok(s1)) return "wildcard-in-topic-name";
        unsigned id = 0;
        if (qos) {
            if (end - p < 2) return "missing-packet-id";
            id = (unsigned)((p[0] << 8) | p[1]);
            if (!id) return "zero-packet-id";
            p += 2;
        }
        snprintf(msg, sizeof msg, "PUBLISH topic=%s qos=%u dup=%u retain=%u id=%u payload=%zu bytes", s1, qos, (pk->flags >> 3) & 1, pk->flags & 1, id, (size_t)(end - p));
        return msg;
    }
    case 8: {
        if (pk->flags != 2) return "bad-subscribe-flags";
        if (end - p < 2) return "short-subscribe";
        unsigned id = (unsigned)((p[0] << 8) | p[1]);
        p += 2;
        size_t o = (size_t)snprintf(msg, sizeof msg, "SUBSCRIBE id=%u", id);
        int cnt = 0;
        while (p < end) {
            if ((e = rd_str(&p, end, s1, sizeof s1))) return e;
            if (p >= end) return "missing-qos";
            if (*p > 2) return "bad-requested-qos";
            if (!filter_ok(s1)) return "bad-topic-filter";
            o += (size_t)snprintf(msg + o, sizeof msg - o, " %s@%u", s1, *p);
            p++; cnt++;
        }
        if (!cnt) return "no-topic-filters";
        return msg;
    }
    case 12: case 13: case 14:
        if (pk->flags != 0 || pk->blen != 0) return "bad-empty-packet";
        return names[pk->type];
    default:
        return "unsupported-type";
    }
}

/* Stream splitter: returns consumed bytes, sets *ok to 1 for a full packet. */
static int next_packet(const unsigned char *p, size_t n, Pkt *pk, size_t *used, const char **err) {
    if (n < 2) return 0;
    uint32_t rl;
    size_t u;
    int r = dec_rl(p + 1, n - 1, &rl, &u);
    if (r == 0) return 0;
    if (r < 0) { *err = r == -1 ? "remaining-length-too-long" : "non-minimal-remaining-length"; return -1; }
    if (n < 1 + u + rl) return 0;
    pk->type = p[0] >> 4; pk->flags = p[0] & 15; pk->body = p + 1 + u; pk->blen = rl;
    *used = 1 + u + rl;
    return 1;
}

int main(void) {
    static const uint32_t rls[] = { 0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455 };
    for (size_t i = 0; i < sizeof rls / sizeof rls[0]; i++) {
        unsigned char b[4];
        size_t n = enc_rl(b, rls[i]);
        uint32_t back; size_t u;
        CHECK(dec_rl(b, n, &back, &u) == 1 && back == rls[i] && u == n);
        printf("remaining length %9u -> %zu byte(s):", (unsigned)rls[i], n);
        for (size_t k = 0; k < n; k++) printf(" %02x", b[k]);
        printf("\n");
    }
    static const unsigned char r5[] = { 0x80, 0x80, 0x80, 0x80, 0x01 }, nm[] = { 0x80, 0x00 }, part[] = { 0x80, 0x80 };
    uint32_t v; size_t u;
    printf("five continuation bytes: %d, non-minimal 0x80 0x00: %d, truncated: %d\n", dec_rl(r5, 5, &v, &u), dec_rl(nm, 2, &v, &u), dec_rl(part, 2, &v, &u));
    for (uint32_t x = 0; x < 300000; x += 7) { unsigned char b[4]; size_t n = enc_rl(b, x); CHECK(dec_rl(b, n, &v, &u) == 1 && v == x); }

    /* Build a stream of packets. */
    static unsigned char stream[4096];
    size_t sn = 0;
    Buf b;
    b.n = 0; bstr(&b, "MQTT"); b8(&b, 4); b8(&b, 0xee); b16(&b, 60); bstr(&b, "client-42"); bstr(&b, "status/client-42"); bstr(&b, "offline"); bstr(&b, "alice"); bstr(&b, "s3cret");
    sn += wrap(stream + sn, 1, 0, &b);
    b.n = 0; bstr(&b, "sensors/kitchen/temp"); b16(&b, 7); memcpy(b.b + b.n, "21.5C", 5); b.n += 5;
    sn += wrap(stream + sn, 3, 0x0b, &b);
    b.n = 0; b16(&b, 10); bstr(&b, "sensors/+/temp"); b8(&b, 1); bstr(&b, "alerts/#"); b8(&b, 0);
    sn += wrap(stream + sn, 8, 2, &b);
    b.n = 0; bstr(&b, "big/topic"); memset(b.b + b.n, 'x', 900); b.n += 900;
    sn += wrap(stream + sn, 3, 0, &b);
    b.n = 0; sn += wrap(stream + sn, 12, 0, &b);
    /* Feed in 5 byte pieces to exercise partial headers. */
    unsigned char acc[4096];
    size_t have = 0, fed = 0;
    int count = 0;
    while (fed < sn) {
        size_t step = sn - fed < 5 ? sn - fed : 5;
        memcpy(acc + have, stream + fed, step);
        have += step; fed += step;
        for (;;) {
            Pkt pk; size_t used; const char *err = NULL;
            int r = next_packet(acc, have, &pk, &used, &err);
            CHECK(r >= 0);
            if (r == 0) break;
            printf("packet %d (%zu bytes): %s\n", ++count, used, describe(&pk));
            memmove(acc, acc + used, have - used);
            have -= used;
        }
    }
    CHECK(count == 5 && have == 0);

    /* Malformed packets. */
    struct { unsigned type, flags; const char *label; } cases[] = { { 3, 6, "publish qos 3" }, { 3, 2, "publish wildcard topic" }, { 8, 0, "subscribe flags 0" }, { 8, 2, "subscribe bad filter" }, { 12, 0, "pingreq with body" }, { 1, 0, "connect wrong name" } };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        b.n = 0;
        if (i == 1) { bstr(&b, "a/+/b"); b16(&b, 1); }
        else if (i == 3) { b16(&b, 1); bstr(&b, "a/b+"); b8(&b, 0); }
        else if (i == 2) { b16(&b, 1); bstr(&b, "a"); b8(&b, 0); }
        else if (i == 4) b8(&b, 0);
        else if (i == 5) { bstr(&b, "MQIsdp"); b8(&b, 3); b8(&b, 2); b16(&b, 10); bstr(&b, "x"); }
        else { bstr(&b, "t"); b16(&b, 1); }
        unsigned char out[128];
        size_t n = wrap(out, cases[i].type, cases[i].flags, &b);
        Pkt pk; size_t used; const char *err = NULL;
        CHECK(next_packet(out, n, &pk, &used, &err) == 1);
        printf("%-24s -> %s\n", cases[i].label, describe(&pk));
    }
    const unsigned char bad_rl[] = { 0x30, 0xff, 0xff, 0xff, 0xff, 0x7f };
    Pkt pk; size_t used; const char *err = NULL;
    CHECK(next_packet(bad_rl, sizeof bad_rl, &pk, &used, &err) == -1);
    printf("overlong remaining length -> %s\n", err);

    /* Topic filter matching table. */
    static const struct { const char *f, *t; int want; } m[] = {
        { "sport/tennis/player1/#", "sport/tennis/player1", 1 }, { "sport/tennis/player1/#", "sport/tennis/player1/ranking", 1 },
        { "sport/tennis/+", "sport/tennis/player1", 1 }, { "sport/tennis/+", "sport/tennis/player1/ranking", 0 },
        { "sport/+", "sport/", 1 }, { "sport/+", "sport", 0 }, { "+/+", "/finance", 1 }, { "/+", "/finance", 1 }, { "+", "/finance", 0 },
        { "#", "any/thing", 1 }, { "#", "$SYS/broker/uptime", 0 }, { "$SYS/#", "$SYS/broker/uptime", 1 }, { "+/monitor/Clients", "$SYS/monitor/Clients", 0 },
        { "a/b/c", "a/b/c", 1 }, { "a/b/c", "a/b", 0 }, { "a/b", "a/b/c", 0 }, { "a/#", "a", 1 },
    };
    int matched = 0;
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int r = match(m[i].f, m[i].t);
        CHECK(r == m[i].want);
        matched += r;
    }
    printf("filter table: %zu cases verified, %d matches\n", sizeof m / sizeof m[0], matched);
    static const char *fbad[] = { "a/b+", "a/#/c", "#a", "", "a++", "+a/b" };
    printf("invalid filters:");
    for (size_t i = 0; i < sizeof fbad / sizeof fbad[0]; i++) { CHECK(!filter_ok(fbad[i])); printf(" '%s'", fbad[i]); }
    printf("\n");
    return 0;
}
