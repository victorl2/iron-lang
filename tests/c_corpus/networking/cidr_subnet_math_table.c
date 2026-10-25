/*
 * title: CIDR parsing and subnet arithmetic
 * topic: networking
 * covers: CIDR parsing, netmask and wildcard, network and broadcast, host range, /31 and /32 rules, RFC 1918 classes, containment, brute force cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { uint32_t addr; int len; } Cidr;

static int parse_ip(const char **pp, uint32_t *out) {
    const char *p = *pp;
    uint32_t acc = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        int nd = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); p++; if (++nd > 3) return 0; }
        if (nd == 0 || v > 255) return 0;
        acc = (acc << 8) | v;
        if (i < 3) { if (*p != '.') return 0; p++; }
    }
    *pp = p;
    *out = acc;
    return 1;
}

/* strict: reject host bits set. Returns 0 ok, 1 syntax, 2 host bits. */
static uint32_t mask_of(int len) { return len == 0 ? 0u : (0xffffffffu << (32 - len)); }

static int parse_cidr(const char *s, Cidr *c, int strict) {
    const char *p = s;
    uint32_t a;
    if (!parse_ip(&p, &a)) return 1;
    if (*p != '/') return 1;
    p++;
    if (*p < '0' || *p > '9') return 1;
    int len = 0, nd = 0;
    while (*p >= '0' && *p <= '9') { len = len * 10 + (*p - '0'); p++; if (++nd > 2) return 1; }
    if (*p || len > 32) return 1;
    if (strict && (a & ~mask_of(len))) return 2;
    c->addr = a & mask_of(len);
    c->len = len;
    return 0;
}

static void ip(uint32_t a, char *b) {
    snprintf(b, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255));
}

static uint32_t net(Cidr c) { return c.addr; }
static uint32_t bcast(Cidr c) { return c.addr | ~mask_of(c.len); }
static uint64_t total(Cidr c) { return 1ull << (32 - c.len); }
static uint64_t hosts(Cidr c) {
    if (c.len == 32) return 1;
    if (c.len == 31) return 2;
    return total(c) - 2;
}
static int contains(Cidr c, uint32_t a) { return (a & mask_of(c.len)) == c.addr; }
static int overlaps(Cidr a, Cidr b) { return contains(a, b.addr) || contains(b, a.addr); }

static const char *klass(uint32_t a) {
    if ((a >> 24) == 10) return "private-10/8";
    if ((a >> 20) == 0xac1) return "private-172.16/12";
    if ((a >> 16) == 0xc0a8) return "private-192.168/16";
    if ((a >> 24) == 127) return "loopback";
    if ((a >> 16) == 0xa9fe) return "link-local";
    if ((a >> 28) == 0xe) return "multicast";
    if ((a >> 28) == 0xf) return "reserved";
    return "public";
}

int main(void) {
    static const char *tab[] = {
        "192.168.10.77/24", "10.0.0.0/8", "172.16.5.9/20", "203.0.113.5/31", "198.51.100.1/32",
        "0.0.0.0/0", "169.254.1.1/16", "224.0.0.9/4", "192.0.2.130/26", "8.8.8.8/29",
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        Cidr c;
        CHECK(parse_cidr(tab[i], &c, 0) == 0);
        char n[16], b[16], m[16], w[16], f[16], l[16];
        ip(net(c), n); ip(bcast(c), b); ip(mask_of(c.len), m); ip(~mask_of(c.len), w);
        if (c.len >= 31) { ip(net(c), f); ip(bcast(c), l); }
        else { ip(net(c) + 1, f); ip(bcast(c) - 1, l); }
        printf("%-18s net=%-15s bcast=%-15s mask=%-15s wild=%-15s hosts=%llu [%s..%s] %s\n", tab[i], n, b, m, w,
               (unsigned long long)hosts(c), f, l, klass(c.addr));
        CHECK(bcast(c) - net(c) + 1 == (uint32_t)total(c) || c.len == 0);
    }
    static const char *bad[] = { "10.0.0.0", "10.0.0.0/", "10.0.0.0/33", "10.0.0.0/-1", "10.0.0/8", "10.0.0.0/008", "10.0.0.256/8", "10.0.0.1/24 " };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Cidr c;
        CHECK(parse_cidr(bad[i], &c, 0) == 1);
    }
    Cidr c;
    CHECK(parse_cidr("10.1.2.3/16", &c, 1) == 2);
    CHECK(parse_cidr("10.1.0.0/16", &c, 1) == 0);
    printf("strict mode rejects host bits; %zu syntax errors detected\n", sizeof bad / sizeof bad[0]);

    /* Pairwise relations among a set. */
    static const char *set[] = { "10.0.0.0/8", "10.20.0.0/16", "10.20.30.0/24", "10.21.0.0/16", "192.168.0.0/16", "0.0.0.0/0" };
    int ns = (int)(sizeof set / sizeof set[0]);
    Cidr cs[6];
    for (int i = 0; i < ns; i++) CHECK(parse_cidr(set[i], &cs[i], 1) == 0);
    for (int i = 0; i < ns; i++) {
        printf("%-16s overlaps:", set[i]);
        for (int j = 0; j < ns; j++) if (i != j && overlaps(cs[i], cs[j])) printf(" %d", j);
        printf("\n");
    }
    /* Brute force: count addresses of small prefixes by scanning a window. */
    uint32_t base = 0xc0a80a00u;
    for (int len = 22; len <= 32; len++) {
        Cidr q = { base & mask_of(len), len };
        unsigned cnt = 0;
        for (uint32_t a = base - 1024; a < base + 2048; a++) if (contains(q, a)) cnt++;
        CHECK(cnt == (unsigned)total(q) || (len < 22));
    }
    printf("window scan agrees with block sizes for /22../32\n");
    return 0;
}
