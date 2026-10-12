/*
 * title: Hostname validation, wildcard certificate matching and reverse DNS names
 * topic: networking
 * covers: RFC 1123 label rules, trailing dot FQDN, numeric TLD rejection, underscore policy, RFC 6125 single-label wildcard matching, case folding, in-addr.arpa and ip6.arpa name generation and parsing
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef enum { H_OK, H_EMPTY, H_TOO_LONG, H_EMPTY_LABEL, H_LABEL_LONG, H_HYPHEN_EDGE, H_BAD_CHAR, H_NUMERIC_TLD, H_IP_LITERAL, H_UNDERSCORE } HErr;
static const char *hname[] = { "ok", "empty", "too-long", "empty-label", "label-too-long", "hyphen-at-label-edge", "bad-character", "all-numeric-tld", "looks-like-ipv4", "underscore-not-allowed" };

static int is_ipv4_text(const char *s) {
    int parts = 0;
    while (1) {
        int nd = 0;
        while (isdigit((unsigned char)*s)) { s++; nd++; }
        if (nd == 0 || nd > 3) return 0;
        parts++;
        if (*s == '.') { s++; continue; }
        return parts == 4 && *s == 0;
    }
}

/* allow_us: permit underscores (service labels such as _dmarc). */
static HErr validate_host(const char *h, int allow_us, int allow_wild) {
    size_t len = strlen(h);
    if (!len) return H_EMPTY;
    if (h[len - 1] == '.') len--; /* absolute name */
    if (!len) return H_EMPTY_LABEL;
    if (len > 253) return H_TOO_LONG;
    if (is_ipv4_text(h)) return H_IP_LITERAL;
    size_t ls = 0;
    int labels = 0;
    int last_numeric = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || h[i] == '.') {
            size_t l = i - ls;
            if (l == 0) return H_EMPTY_LABEL;
            if (l > 63) return H_LABEL_LONG;
            int wild = allow_wild && labels == 0 && l == 1 && h[ls] == '*';
            if (!wild && (h[ls] == '-' || h[i - 1] == '-')) return H_HYPHEN_EDGE;
            int num = 1;
            for (size_t k = ls; k < i; k++) {
                unsigned char c = (unsigned char)h[k];
                if (!isdigit(c)) num = 0;
                if (wild) continue;
                if (c == '_') { if (!allow_us) return H_UNDERSCORE; continue; }
                if (!isalnum(c) && c != '-') return H_BAD_CHAR;
            }
            last_numeric = num;
            labels++;
            ls = i + 1;
        }
    }
    if (labels > 1 && last_numeric) return H_NUMERIC_TLD;
    return H_OK;
}

static int ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* RFC 6125 6.4.3: wildcard only as the entire leftmost label, matches exactly one label, needs at least two labels after it. */
static int cert_match(const char *pattern, const char *host) {
    size_t hl = strlen(host), pl = strlen(pattern);
    if (hl && host[hl - 1] == '.') hl--;
    if (pl && pattern[pl - 1] == '.') pl--;
    if (pl >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        const char *rest = pattern + 1; /* ".example.com" */
        size_t rl = pl - 1;
        int dots = 0;
        for (size_t i = 0; i < rl; i++) if (rest[i] == '.') dots++;
        if (dots < 2) return 0; /* *.com is not allowed */
        const char *dot = memchr(host, '.', hl);
        if (!dot || dot == host) return 0;
        size_t tail = hl - (size_t)(dot - host);
        return tail == rl && ieq_n(dot, rest, rl);
    }
    if (memchr(pattern, '*', pl)) return 0;
    return pl == hl && ieq_n(pattern, host, hl);
}

static void arpa_v4(uint32_t a, char *o) {
    snprintf(o, 40, "%u.%u.%u.%u.in-addr.arpa", (unsigned)(a & 255), (unsigned)((a >> 8) & 255), (unsigned)((a >> 16) & 255), (unsigned)(a >> 24));
}
static void arpa_v6(const unsigned char *a, char *o) {
    static const char hx[] = "0123456789abcdef";
    size_t k = 0;
    for (int i = 15; i >= 0; i--) { o[k++] = hx[a[i] & 15]; o[k++] = '.'; o[k++] = hx[a[i] >> 4]; o[k++] = '.'; }
    strcpy(o + k, "ip6.arpa");
}
static int parse_arpa_v4(const char *s, uint32_t *a) {
    unsigned p[4];
    char tail[20];
    if (sscanf(s, "%u.%u.%u.%u.%19s", &p[0], &p[1], &p[2], &p[3], tail) != 5) return 0;
    if (strcmp(tail, "in-addr.arpa") != 0) return 0;
    for (int i = 0; i < 4; i++) if (p[i] > 255) return 0;
    *a = (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
    return 1;
}
static int parse_arpa_v6(const char *s, unsigned char *a) {
    size_t l = strlen(s);
    if (l != 72 || strcmp(s + 64, "ip6.arpa") != 0) return 0;
    for (int i = 0; i < 32; i++) {
        char c = s[i * 2];
        if (!isxdigit((unsigned char)c) || s[i * 2 + 1] != '.') return 0;
        int v = isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
        int byte = 15 - i / 2;
        if (i & 1) a[byte] = (unsigned char)((a[byte] & 15) | (v << 4)); else a[byte] = (unsigned char)((a[byte] & 0xf0) | v);
    }
    return 1;
}

int main(void) {
    static const char *hosts[] = {
        "example.com", "www.Example.COM", "a.b.c.d.e.f.example.org", "localhost", "my-host-1.example.net", "xn--bcher-kva.example", "example.com.", "1.example.com",
        "3com.com", "-bad.example.com", "bad-.example.com", "a..b.com", ".example.com", "exa_mple.com", "_dmarc.example.com", "ex ample.com", "example.123", "192.168.1.1",
        "*.example.com", "host*.example.com", "", ".", "under\xc3\xa9.com", "a-b.c-d.e-f",
    };
    int ok = 0;
    for (size_t i = 0; i < sizeof hosts / sizeof hosts[0]; i++) {
        HErr e = validate_host(hosts[i], 0, 0);
        HErr e2 = validate_host(hosts[i], 1, 1);
        printf("%-26s %-22s%s\n", hosts[i][0] ? hosts[i] : "(empty)", hname[e], e != e2 ? (e2 == H_OK ? "  (ok with underscore/wildcard policy)" : "  (differs under policy)") : "");
        ok += e == H_OK;
    }
    printf("%d valid hostnames\n", ok);
    /* Length limits. */
    char big[300], lab[64];
    memset(lab, 'x', 63); lab[63] = 0;
    snprintf(big, sizeof big, "%s.%s.%s.%.61s", lab, lab, lab, lab);
    printf("253 char name: %s (len %zu)\n", hname[validate_host(big, 0, 0)], strlen(big));
    strcat(big, "y");
    printf("254 char name: %s (len %zu)\n", hname[validate_host(big, 0, 0)], strlen(big));
    snprintf(big, sizeof big, "%sz.com", lab);
    printf("64 char label: %s\n", hname[validate_host(big, 0, 0)]);

    static const struct { const char *pat, *host; int want; } m[] = {
        { "example.com", "EXAMPLE.com", 1 }, { "*.example.com", "www.example.com", 1 }, { "*.example.com", "example.com", 0 }, { "*.example.com", "a.b.example.com", 0 },
        { "*.example.com", "WWW.EXAMPLE.COM.", 1 }, { "*.com", "example.com", 0 }, { "w*.example.com", "www.example.com", 0 }, { "www.*.com", "www.a.com", 0 },
        { "*.example.com", ".example.com", 0 }, { "*.example.co.uk", "shop.example.co.uk", 1 }, { "example.com", "www.example.com", 0 }, { "*.a.b.c", "x.a.b.c", 1 },
    };
    int hits = 0;
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int r = cert_match(m[i].pat, m[i].host);
        CHECK(r == m[i].want);
        hits += r;
    }
    printf("certificate matching: %zu cases verified, %d matches\n", sizeof m / sizeof m[0], hits);

    /* Reverse names. */
    char rn[100];
    arpa_v4(0xc0000201u, rn);
    printf("192.0.2.1 -> %s\n", rn);
    CHECK(strcmp(rn, "1.2.0.192.in-addr.arpa") == 0);
    uint32_t back;
    CHECK(parse_arpa_v4(rn, &back) && back == 0xc0000201u);
    CHECK(!parse_arpa_v4("1.2.3.256.in-addr.arpa", &back) && !parse_arpa_v4("1.2.3.in-addr.arpa", &back));
    unsigned char v6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01 };
    arpa_v6(v6, rn);
    printf("2001:db8::1 -> %s\n", rn);
    CHECK(strcmp(rn, "1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa") == 0);
    unsigned char b6[16] = { 0 };
    CHECK(parse_arpa_v6(rn, b6) && memcmp(b6, v6, 16) == 0);
    /* Round trip random addresses. */
    uint32_t rs = 0x1234abcdu;
    for (int t = 0; t < 1000; t++) {
        unsigned char a[16], z[16] = { 0 };
        for (int i = 0; i < 16; i++) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; a[i] = (unsigned char)(rs >> 9); }
        arpa_v6(a, rn);
        CHECK(strlen(rn) == 72 && parse_arpa_v6(rn, z) && memcmp(a, z, 16) == 0);
        uint32_t x = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3], y;
        arpa_v4(x, rn);
        CHECK(parse_arpa_v4(rn, &y) && x == y);
    }
    printf("1000 random IPv4 and IPv6 reverse names round-trip; ip6.arpa names are 72 characters\n");
    return 0;
}
