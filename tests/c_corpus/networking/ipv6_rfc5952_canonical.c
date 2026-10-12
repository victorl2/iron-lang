/*
 * title: IPv6 parser and RFC 5952 canonical formatter
 * topic: networking
 * covers: IPv6 text parsing, zero compression, longest run tie break, embedded IPv4, canonical lowercase form, inet_pton and inet_ntop cross-check
 * deps: libc, sockets
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_v4tail(const char *s, uint16_t *hi, uint16_t *lo) {
    unsigned v[4];
    int n = 0;
    const char *p = s;
    while (n < 4) {
        int nd = 0;
        unsigned x = 0;
        const char *st = p;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (unsigned)(*p - '0'); p++; nd++; if (nd > 3) return 0; }
        if (nd == 0 || x > 255 || (nd > 1 && *st == '0')) return 0;
        v[n++] = x;
        if (n < 4) { if (*p != '.') return 0; p++; }
    }
    if (*p) return 0;
    *hi = (uint16_t)((v[0] << 8) | v[1]);
    *lo = (uint16_t)((v[2] << 8) | v[3]);
    return 1;
}

/* Returns 1 on success and fills 8 groups. */
static int parse6(const char *s, uint16_t g[8]) {
    uint16_t head[8], tail[8];
    int nh = 0, nt = 0, seen = 0;
    const char *p = s;
    if (p[0] == ':') {
        if (p[1] != ':') return 0;
        seen = 1;
        p += 2;
        if (!*p) { memset(g, 0, 16); return 1; }
    }
    for (;;) {
        /* one group or embedded IPv4 */
        const char *q = p;
        while (*q && *q != ':') q++;
        int len = (int)(q - p);
        int has_dot = 0;
        for (const char *t = p; t < q; t++) if (*t == '.') has_dot = 1;
        uint16_t *arr = seen ? tail : head;
        int *cnt = seen ? &nt : &nh;
        if (has_dot) {
            if (*q) return 0;
            if (*cnt + 2 > 8) return 0;
            if (!parse_v4tail(p, &arr[*cnt], &arr[*cnt + 1])) return 0;
            *cnt += 2;
            p = q;
            break;
        }
        if (len < 1 || len > 4) return 0;
        unsigned v = 0;
        for (int i = 0; i < len; i++) {
            int h = hexval(p[i]);
            if (h < 0) return 0;
            v = v * 16 + (unsigned)h;
        }
        if (*cnt >= 8) return 0;
        arr[(*cnt)++] = (uint16_t)v;
        p = q;
        if (!*p) break;
        p++; /* skip ':' */
        if (*p == ':') {
            if (seen) return 0;
            seen = 1;
            p++;
            if (!*p) break;
        } else if (!*p) {
            return 0; /* trailing single colon */
        }
    }
    if (seen) {
        if (nh + nt > 7) return 0;
        memset(g, 0, 16);
        for (int i = 0; i < nh; i++) g[i] = head[i];
        for (int i = 0; i < nt; i++) g[8 - nt + i] = tail[i];
    } else {
        if (nh != 8) return 0;
        memcpy(g, head, 16);
    }
    return 1;
}

static void format6(const uint16_t g[8], char *out) {
    int bs = -1, bl = 0;
    for (int i = 0; i < 8;) {
        if (g[i] != 0) { i++; continue; }
        int j = i;
        while (j < 8 && g[j] == 0) j++;
        if (j - i > bl) { bl = j - i; bs = i; }
        i = j;
    }
    if (bl < 2) bs = -1;
    char *o = out;
    for (int i = 0; i < 8; i++) {
        if (i == bs) {
            *o++ = ':';
            if (i == 0) *o++ = ':';
            i += bl - 1;
            continue;
        }
        o += snprintf(o, 6, "%x", (unsigned)g[i]);
        if (i != 7) *o++ = ':';
    }
    *o = 0;
}

static uint32_t rs = 0x2545f491u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    static const struct { const char *in; const char *canon; } valid[] = {
        { "::", "::" }, { "::1", "::1" }, { "1::", "1::" }, { "2001:db8::1", "2001:db8::1" },
        { "2001:0DB8:0000:0000:0000:0000:0000:0001", "2001:db8::1" },
        { "2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1" },
        { "2001:0:0:1:0:0:0:1", "2001:0:0:1::1" },
        { "2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1" },
        { "1:2:3:4:5:6:7::", "1:2:3:4:5:6:7:0" },
        { "::2:3:4:5:6:7:8", "0:2:3:4:5:6:7:8" },
        { "1:0:0:2:0:0:3:4", "1::2:0:0:3:4" },
        { "::ffff:192.0.2.128", "::ffff:c000:280" },
        { "64:ff9b::198.51.100.7", "64:ff9b::c633:6407" },
        { "FE80::A:B", "fe80::a:b" },
        { "0:0:0:0:0:0:0:0", "::" },
        { "1:0:0:0:0:0:0:8", "1::8" },
        { "0:0:1:0:0:0:0:0", "0:0:1::" },
    };
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        uint16_t g[8];
        char out[48];
        CHECK(parse6(valid[i].in, g));
        format6(g, out);
        printf("%-42s -> %s\n", valid[i].in, out);
        CHECK(strcmp(out, valid[i].canon) == 0);
        struct in6_addr a;
        CHECK(inet_pton(AF_INET6, valid[i].in, &a) == 1);
        for (int k = 0; k < 8; k++) CHECK(((a.s6_addr[2 * k] << 8) | a.s6_addr[2 * k + 1]) == g[k]);
    }
    static const char *bad[] = {
        ":", ":::", "1:::2", "1::2::3", "12345::", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", ":1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:", "g::1", "1:2:3:4:5:6:7:8::", "::1.2.3", "::1.2.3.4.5", "1.2.3.4::", "::256.1.1.1",
        "", "1::2:", "::ffff:1.2.3.4:5",
    };
    int nbad = (int)(sizeof bad / sizeof bad[0]);
    for (int i = 0; i < nbad; i++) {
        uint16_t g[8];
        struct in6_addr a;
        CHECK(!parse6(bad[i], g));
        CHECK(inet_pton(AF_INET6, bad[i], &a) != 1);
    }
    printf("rejected %d malformed strings\n", nbad);

    /* Random addresses: canonical form must match inet_ntop when no mapped-IPv4 rule applies. */
    int runs = 0, longest = 0;
    for (int it = 0; it < 5000; it++) {
        uint16_t g[8];
        for (int k = 0; k < 8; k++) {
            uint32_t r = rnd();
            g[k] = (r & 3) ? 0 : (uint16_t)(r >> 8);
        }
        if (g[0] == 0) g[0] = (uint16_t)(1 + (rnd() % 0xfff0u));
        char mine[48], lib[INET6_ADDRSTRLEN];
        format6(g, mine);
        struct in6_addr a;
        for (int k = 0; k < 8; k++) { a.s6_addr[2 * k] = (unsigned char)(g[k] >> 8); a.s6_addr[2 * k + 1] = (unsigned char)g[k]; }
        CHECK(inet_ntop(AF_INET6, &a, lib, sizeof lib) != NULL);
        CHECK(strcmp(mine, lib) == 0);
        uint16_t back[8];
        CHECK(parse6(mine, back) && memcmp(back, g, 16) == 0);
        if (strstr(mine, "::")) runs++;
        if ((int)strlen(mine) > longest) longest = (int)strlen(mine);
    }
    printf("random addresses with :: compression: %d of 5000, longest text %d chars\n", runs, longest);
    return 0;
}
