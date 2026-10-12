/*
 * title: IPv6 address classification, EUI-64 and embedded IPv4 forms
 * topic: networking
 * covers: prefix table classification, multicast flags and scope, solicited-node addresses, multicast MAC mapping, modified EUI-64 interface identifiers, 6to4 Teredo and NAT64 embedding, 128 bit prefix arithmetic
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { unsigned char b[16]; } A6;

static int hexv(int c) { return isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }

/* Compact parser: hex groups and one "::" (no embedded IPv4 here). */
static int parse(const char *s, A6 *out) {
    unsigned g[8], tail[8];
    int ng = 0, nt = 0, dbl = 0;
    const char *p = s;
    if (p[0] == ':' && p[1] == ':') { dbl = 1; p += 2; }
    else if (p[0] == ':') return 0;
    while (*p) {
        int nd = 0;
        unsigned v = 0;
        while (hexv(*p) >= 0) { v = v * 16 + (unsigned)hexv(*p); p++; if (++nd > 4) return 0; }
        if (!nd) return 0;
        if (dbl) { if (nt == 8) return 0; tail[nt++] = v; } else { if (ng == 8) return 0; g[ng++] = v; }
        if (*p == ':') {
            p++;
            if (*p == ':') { if (dbl) return 0; dbl = 1; p++; }
            else if (!*p) return 0;
        } else if (*p) return 0;
    }
    if (dbl ? ng + nt > 7 : ng != 8) return 0;
    unsigned all[8] = { 0 };
    for (int i = 0; i < ng; i++) all[i] = g[i];
    for (int i = 0; i < nt; i++) all[8 - nt + i] = tail[i];
    for (int i = 0; i < 8; i++) { out->b[2 * i] = (unsigned char)(all[i] >> 8); out->b[2 * i + 1] = (unsigned char)all[i]; }
    return 1;
}

static void fmt(const A6 *a, char *o) {
    unsigned g[8];
    for (int i = 0; i < 8; i++) g[i] = (unsigned)((a->b[2 * i] << 8) | a->b[2 * i + 1]);
    int bs = -1, bl = 0;
    for (int i = 0; i < 8;) {
        if (g[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !g[j]) j++;
        if (j - i > bl) { bl = j - i; bs = i; }
        i = j;
    }
    if (bl < 2) bs = -1;
    size_t k = 0;
    for (int i = 0; i < 8; i++) {
        if (i == bs) { o[k++] = ':'; if (i == 0) o[k++] = ':'; i += bl - 1; continue; }
        k += (size_t)snprintf(o + k, 8, "%x", g[i]);
        if (i != 7) o[k++] = ':';
    }
    o[k] = 0;
}

static int prefix_match(const A6 *a, const A6 *p, int len) {
    for (int i = 0; i < len / 8; i++) if (a->b[i] != p->b[i]) return 0;
    if (len % 8) {
        unsigned char m = (unsigned char)(0xff << (8 - len % 8));
        if ((a->b[len / 8] & m) != (p->b[len / 8] & m)) return 0;
    }
    return 1;
}

static const struct { const char *pfx; int len; const char *name; } classes[] = {
    { "::", 128, "unspecified" }, { "::1", 128, "loopback" }, { "::ffff:0:0", 96, "ipv4-mapped" }, { "64:ff9b::", 96, "nat64" }, { "100::", 64, "discard-only" },
    { "2001::", 32, "teredo" }, { "2001:db8::", 32, "documentation" }, { "2002::", 16, "6to4" }, { "fc00::", 7, "unique-local" }, { "fe80::", 10, "link-local" },
    { "ff00::", 8, "multicast" }, { "2000::", 3, "global-unicast" },
};

static const char *classify(const A6 *a) {
    int best = -1, bl = -1;
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        A6 p;
        CHECK(parse(classes[i].pfx, &p));
        if (prefix_match(a, &p, classes[i].len) && classes[i].len > bl) { bl = classes[i].len; best = (int)i; }
    }
    return best < 0 ? "reserved" : classes[best].name;
}

static void eui64_iid(const unsigned char mac[6], unsigned char iid[8]) {
    iid[0] = (unsigned char)(mac[0] ^ 0x02); iid[1] = mac[1]; iid[2] = mac[2]; iid[3] = 0xff; iid[4] = 0xfe; iid[5] = mac[3]; iid[6] = mac[4]; iid[7] = mac[5];
}
static int mac_from_iid(const unsigned char iid[8], unsigned char mac[6]) {
    if (iid[3] != 0xff || iid[4] != 0xfe) return 0;
    mac[0] = (unsigned char)(iid[0] ^ 0x02); mac[1] = iid[1]; mac[2] = iid[2]; mac[3] = iid[5]; mac[4] = iid[6]; mac[5] = iid[7];
    return 1;
}

static void describe_multicast(const A6 *a, char *o, size_t cap) {
    unsigned flags = a->b[1] >> 4, scope = a->b[1] & 15;
    static const char *sc[16] = { "reserved", "interface-local", "link-local", "reserved", "admin-local", "site-local", "unassigned", "unassigned", "organization-local", "unassigned", "unassigned", "unassigned", "unassigned", "unassigned", "global", "reserved" };
    const char *kind = "";
    A6 t;
    parse("ff02::1", &t); if (memcmp(a, &t, 16) == 0) kind = " all-nodes";
    parse("ff02::2", &t); if (memcmp(a, &t, 16) == 0) kind = " all-routers";
    parse("ff02::1:ff00:0", &t);
    if (prefix_match(a, &t, 104)) kind = " solicited-node";
    snprintf(o, cap, "flags=%c%c%c%c scope=%s%s", (flags & 8) ? 'X' : '-', (flags & 4) ? 'R' : '-', (flags & 2) ? 'P' : '-', (flags & 1) ? 'T' : '-', sc[scope], kind);
}

static uint32_t rs = 0x36363636u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    static const char *tests[] = {
        "::", "::1", "::ffff:c000:280", "64:ff9b::c633:6407", "100::1", "2001:0:4136:e378:8000:63bf:3fff:fdd2", "2001:db8:1234::1", "2002:c000:204::1",
        "fd12:3456:789a::1", "fe80::21b:63ff:fe84:45e6", "ff02::1", "ff02::2", "ff02::1:ff23:4567", "ff05::101", "ff3e:40:2001:db8::1234", "2606:4700::6810:84e5",
        "4000::1", "fec0::1", "::2", "ffff::",
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        A6 a;
        CHECK(parse(tests[i], &a));
        char f[48], extra[100] = "";
        fmt(&a, f);
        const char *c = classify(&a);
        if (strcmp(c, "multicast") == 0) describe_multicast(&a, extra, sizeof extra);
        else if (strcmp(c, "ipv4-mapped") == 0) snprintf(extra, sizeof extra, "v4=%u.%u.%u.%u", a.b[12], a.b[13], a.b[14], a.b[15]);
        else if (strcmp(c, "nat64") == 0) snprintf(extra, sizeof extra, "v4=%u.%u.%u.%u", a.b[12], a.b[13], a.b[14], a.b[15]);
        else if (strcmp(c, "6to4") == 0) snprintf(extra, sizeof extra, "gateway v4=%u.%u.%u.%u", a.b[2], a.b[3], a.b[4], a.b[5]);
        else if (strcmp(c, "teredo") == 0) {
            unsigned port = (unsigned)(((a.b[10] << 8) | a.b[11]) ^ 0xffff);
            snprintf(extra, sizeof extra, "server=%u.%u.%u.%u client=%u.%u.%u.%u:%u", a.b[4], a.b[5], a.b[6], a.b[7], a.b[12] ^ 0xff, a.b[13] ^ 0xff, a.b[14] ^ 0xff, a.b[15] ^ 0xff, port);
        }
        else if (strcmp(c, "link-local") == 0 || strcmp(c, "global-unicast") == 0) {
            unsigned char mac[6];
            if (mac_from_iid(a.b + 8, mac)) snprintf(extra, sizeof extra, "EUI-64 from mac %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        printf("%-40s %-24s %s\n", f, c, extra);
        A6 back;
        CHECK(parse(f, &back) && memcmp(&back, &a, 16) == 0);
    }
    /* EUI-64 derivation and solicited-node / multicast MAC. */
    static const unsigned char mac[6] = { 0x00, 0x1b, 0x63, 0x84, 0x45, 0xe6 };
    unsigned char iid[8], mac2[6];
    eui64_iid(mac, iid);
    CHECK(mac_from_iid(iid, mac2) && memcmp(mac, mac2, 6) == 0);
    A6 ll;
    memset(&ll, 0, sizeof ll);
    ll.b[0] = 0xfe; ll.b[1] = 0x80;
    memcpy(ll.b + 8, iid, 8);
    char f[48];
    fmt(&ll, f);
    printf("MAC 00:1b:63:84:45:e6 -> link-local %s\n", f);
    CHECK(strcmp(f, "fe80::21b:63ff:fe84:45e6") == 0);
    A6 sn;
    parse("ff02::1:ff00:0", &sn);
    memcpy(sn.b + 13, ll.b + 13, 3);
    fmt(&sn, f);
    printf("solicited-node multicast: %s\n", f);
    CHECK(strcmp(f, "ff02::1:ff84:45e6") == 0);
    printf("multicast MAC for it: 33:33:%02x:%02x:%02x:%02x\n", sn.b[12], sn.b[13], sn.b[14], sn.b[15]);
    CHECK(sn.b[12] == 0xff && sn.b[13] == 0x84);
    /* 6to4 address for 192.0.2.4 and its /48. */
    A6 s6;
    memset(&s6, 0, sizeof s6);
    s6.b[0] = 0x20; s6.b[1] = 0x02; s6.b[2] = 192; s6.b[3] = 0; s6.b[4] = 2; s6.b[5] = 4;
    fmt(&s6, f);
    printf("6to4 prefix for 192.0.2.4: %s/48\n", f);
    CHECK(strcmp(f, "2002:c000:204::") == 0);

    /* Prefix arithmetic: count /64 subnets in a /48 and check membership on random data. */
    A6 net;
    parse("2001:db8:abcd::", &net);
    long inside = 0, total = 5000;
    for (long t = 0; t < total; t++) {
        A6 a;
        memcpy(&a, &net, 16);
        int mode = (int)(rnd() % 3);
        for (int i = 6; i < 16; i++) a.b[i] = (unsigned char)rnd();
        if (mode == 0) a.b[5] ^= (unsigned char)(1u << (rnd() % 8)); /* leave the /48 sometimes */
        int m = prefix_match(&a, &net, 48);
        int expect = a.b[5] == net.b[5];
        CHECK(m == expect);
        inside += m;
    }
    printf("random addresses inside 2001:db8:abcd::/48: %ld of %ld; /64 subnets in a /48: %u\n", inside, total, 1u << 16);
    /* every prefix length against a brute force bit loop */
    for (int len = 0; len <= 128; len += 7) {
        A6 a, p;
        for (int i = 0; i < 16; i++) { a.b[i] = (unsigned char)rnd(); p.b[i] = (unsigned char)rnd(); }
        for (int i = 0; i < len; i++) { int bit = (a.b[i / 8] >> (7 - i % 8)) & 1; p.b[i / 8] = (unsigned char)((p.b[i / 8] & ~(0x80 >> (i % 8))) | (bit << (7 - i % 8))); }
        CHECK(prefix_match(&a, &p, len));
        if (len > 0) { p.b[(len - 1) / 8] ^= (unsigned char)(0x80 >> ((len - 1) % 8)); CHECK(!prefix_match(&a, &p, len)); }
    }
    printf("prefix matching verified for lengths 0..128 in steps of 7\n");
    return 0;
}
