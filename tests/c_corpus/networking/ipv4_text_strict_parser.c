/*
 * title: Strict IPv4 text parser versus lenient inet_aton forms
 * topic: networking
 * covers: dotted quad parsing, leading zero rejection, octal ambiguity, hex and short forms, error classification, inet_pton cross-check
 * deps: libc, sockets
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef enum { E_OK, E_EMPTY, E_BAD_CHAR, E_LEADING_ZERO, E_RANGE, E_TOO_FEW, E_TOO_MANY, E_EMPTY_PART, E_LONG_PART } Err;
static const char *ename[] = { "ok", "empty", "bad-char", "leading-zero", "range", "too-few", "too-many", "empty-part", "long-part" };

static Err parse_strict(const char *s, uint32_t *out) {
    if (!*s) return E_EMPTY;
    uint32_t acc = 0;
    int parts = 0;
    const char *p = s;
    for (;;) {
        const char *st = p;
        unsigned v = 0;
        int nd = 0;
        while (*p >= '0' && *p <= '9') {
            if (nd < 4) v = v * 10 + (unsigned)(*p - '0');
            nd++;
            p++;
        }
        if (nd == 0) return (*p == 0 || *p == '.') ? E_EMPTY_PART : E_BAD_CHAR;
        if (nd > 3) return E_LONG_PART;
        if (nd > 1 && *st == '0') return E_LEADING_ZERO;
        if (v > 255) return E_RANGE;
        acc = (acc << 8) | v;
        parts++;
        if (*p == '.') {
            if (parts == 4) return E_TOO_MANY;
            p++;
            continue;
        }
        if (*p == 0) break;
        return E_BAD_CHAR;
    }
    if (parts < 4) return E_TOO_FEW;
    *out = acc;
    return E_OK;
}

/* inet_aton style: 1 to 4 parts, each decimal, 0octal or 0xhex; last part fills the rest. */
static int parse_lenient(const char *s, uint32_t *out) {
    uint64_t vals[4];
    int n = 0;
    const char *p = s;
    if (!*p) return 0;
    for (;;) {
        int base = 10;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (p[0] == '0' && p[1] != 0 && p[1] != '.') { base = 8; p++; }
        uint64_t v = 0;
        int nd = 0;
        for (;; p++, nd++) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else break;
            if (d >= base) return 0;
            v = v * (uint64_t)base + (uint64_t)d;
            if (v > 0xffffffffull) return 0;
        }
        if (nd == 0 && !(base == 8 && p[-1] == '0')) return 0;
        if (n == 4) return 0;
        vals[n++] = v;
        if (*p == '.') { p++; continue; }
        if (*p == 0) break;
        return 0;
    }
    uint32_t acc = 0;
    for (int i = 0; i < n - 1; i++) {
        if (vals[i] > 255) return 0;
        acc |= (uint32_t)vals[i] << (24 - 8 * i);
    }
    int rem = 4 - (n - 1);
    uint64_t maxlast = (rem == 4) ? 0xffffffffull : ((1ull << (8 * rem)) - 1);
    if (vals[n - 1] > maxlast) return 0;
    acc |= (uint32_t)vals[n - 1];
    *out = acc;
    return 1;
}

static void fmt(uint32_t a, char *buf) {
    snprintf(buf, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

static uint32_t rs = 0x1badf00du;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    static const char *tab[] = {
        "192.168.1.1", "0.0.0.0", "255.255.255.255", "010.0.0.1", "1.2.3", "1.2.3.4.5", "256.1.1.1",
        "1..2.3", "1.2.3.", ".1.2.3", "1.2.3.4 ", " 1.2.3.4", "0x7f.0.0.1", "127.1", "0177.0.0.1",
        "08.1.1.1", "1.2.3.-4", "1234.1.1.1", "", "00", "0.0.0.00", "3232235777", "1.2.3.4a", "99.99.99.99"
    };
    int nt = (int)(sizeof tab / sizeof tab[0]);
    int ok = 0;
    for (int i = 0; i < nt; i++) {
        uint32_t a = 0, b = 0;
        Err e = parse_strict(tab[i], &a);
        int len = parse_lenient(tab[i], &b);
        char sa[16], sb[16];
        struct in_addr ia;
        int pt = inet_pton(AF_INET, tab[i], &ia);
        /* libc parsers disagree on leading zeros (macOS accepts them), so skip those. */
        if (e != E_LEADING_ZERO && e != E_LONG_PART) CHECK((pt == 1) == (e == E_OK));
        if (e == E_OK) {
            CHECK(ntohl(ia.s_addr) == a);
            fmt(a, sa);
            ok++;
        } else {
            snprintf(sa, sizeof sa, "-");
        }
        if (len) fmt(b, sb); else snprintf(sb, sizeof sb, "-");
        const char *note = "";
        if (e == E_OK) CHECK(len && a == b);
        if (e != E_OK && len) note = (e == E_LEADING_ZERO) ? "  ambiguous (octal vs decimal)" : "  lenient only";
        printf("%-14s strict=%-13s %-15s lenient=%s%s\n", tab[i][0] ? tab[i] : "(empty)", ename[e], sa, sb, note);
    }
    printf("accepted %d of %d\n", ok, nt);

    /* Round trips of random addresses. */
    for (int i = 0; i < 2000; i++) {
        uint32_t a = rnd(), b = 0;
        char buf[16];
        fmt(a, buf);
        CHECK(parse_strict(buf, &b) == E_OK && a == b);
        CHECK(parse_lenient(buf, &b) && a == b);
    }
    /* Random strings over a small alphabet must agree with inet_pton. */
    static const char alpha[] = "0123456789..";
    int agree = 0, acc = 0;
    for (int i = 0; i < 20000; i++) {
        char buf[20];
        uint32_t r = rnd();
        int len = 1 + (int)(r % 15);
        for (int k = 0; k < len; k++) {
            uint32_t c = rnd();
            buf[k] = alpha[c % 12];
        }
        buf[len] = 0;
        uint32_t a = 0;
        struct in_addr ia;
        Err er = parse_strict(buf, &a);
        int e = er == E_OK;
        int pt = inet_pton(AF_INET, buf, &ia) == 1;
        if (er == E_LEADING_ZERO || er == E_LONG_PART) continue;
        CHECK(e == pt);
        agree++;
        acc += e;
        if (e) CHECK(ntohl(ia.s_addr) == a);
    }
    printf("random strings agree with inet_pton: %d, accepted %d\n", agree, acc);
    return 0;
}
