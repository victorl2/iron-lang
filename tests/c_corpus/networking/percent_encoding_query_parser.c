/*
 * title: Percent-encoding, form decoding and query string parsing
 * topic: networking
 * covers: RFC 3986 unreserved set, strict percent decoding, plus as space in forms, query pair splitting, repeated keys, sorted canonical rebuild, byte fuzz round trip
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static int unreserved(unsigned c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

/* form=1 encodes space as '+', otherwise as %20 */
static size_t pct_encode(const unsigned char *in, size_t n, char *out, int form) {
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = in[i];
        if (unreserved(c)) out[o++] = (char)c;
        else if (c == ' ' && form) out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hx[c >> 4]; out[o++] = hx[c & 15]; }
    }
    out[o] = 0;
    return o;
}

static int hv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* returns decoded length or -1 on malformed escape */
static long pct_decode(const char *in, size_t n, unsigned char *out, int form) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '%') {
            if (i + 2 >= n) return -1;
            int a = hv(in[i + 1]), b = hv(in[i + 2]);
            if (a < 0 || b < 0) return -1;
            out[o++] = (unsigned char)(a * 16 + b);
            i += 2;
        } else if (in[i] == '+' && form) {
            out[o++] = ' ';
        } else {
            out[o++] = (unsigned char)in[i];
        }
    }
    return (long)o;
}

typedef struct { char key[48], val[96]; int has_eq; } Pair;

static int parse_query(const char *q, Pair *ps, int max) {
    int n = 0;
    const char *p = q;
    while (*p) {
        size_t l = strcspn(p, "&");
        if (l > 0) {
            CHECK(n < max);
            const char *eq = memchr(p, '=', l);
            size_t kl = eq ? (size_t)(eq - p) : l;
            unsigned char tmp[128];
            long d = pct_decode(p, kl, tmp, 1);
            if (d < 0) return -1;
            memcpy(ps[n].key, tmp, (size_t)d);
            ps[n].key[d] = 0;
            ps[n].has_eq = eq != NULL;
            ps[n].val[0] = 0;
            if (eq) {
                d = pct_decode(eq + 1, l - kl - 1, tmp, 1);
                if (d < 0) return -1;
                memcpy(ps[n].val, tmp, (size_t)d);
                ps[n].val[d] = 0;
            }
            n++;
        }
        p += l;
        if (*p == '&') p++;
    }
    return n;
}

static int cmp_pair(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    return strcmp(x->val, y->val);
}

static uint32_t rs = 0xc0ffee11u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    char enc[512];
    static const char *samples[] = { "hello world", "a+b=c&d", "caf\xc3\xa9 \xe2\x82\xac", "~-._ok", "100% legit/path?x#y", "" };
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        pct_encode((const unsigned char *)samples[i], strlen(samples[i]), enc, 0);
        char form[512];
        pct_encode((const unsigned char *)samples[i], strlen(samples[i]), form, 1);
        printf("[%s] -> %s | form: %s\n", samples[i], enc[0] ? enc : "(empty)", form[0] ? form : "(empty)");
        unsigned char back[512];
        long d = pct_decode(enc, strlen(enc), back, 0);
        CHECK(d == (long)strlen(samples[i]) && memcmp(back, samples[i], (size_t)d) == 0);
        d = pct_decode(form, strlen(form), back, 1);
        CHECK(d == (long)strlen(samples[i]) && memcmp(back, samples[i], (size_t)d) == 0);
    }
    /* Strict decoding failures and leniency of case / plus. */
    static const char *bad[] = { "%", "%4", "%zz", "abc%g0", "%%41" };
    printf("bad escapes:");
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        unsigned char b[16];
        CHECK(pct_decode(bad[i], strlen(bad[i]), b, 0) < 0);
        printf(" %s", bad[i]);
    }
    printf("\n");
    unsigned char b[16];
    long d = pct_decode("%e2%82%AC+%41", 13, b, 1);
    printf("mixed-case decode: %ld bytes, '+' as space in form mode: [%.*s]\n", d, (int)d, (const char *)b);
    d = pct_decode("a+b", 3, b, 0);
    CHECK(d == 3 && b[1] == '+');

    /* Query strings. */
    static const char *queries[] = {
        "q=iron+lang&page=2&tag=c&tag=net&flag&empty=&x=%3D%26",
        "&&a=1&&b&",
        "name=J%C3%BCrgen&city=Z%C3%BCrich",
        "k=v=w&=novalue&%41=upper",
    };
    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; i++) {
        Pair ps[16];
        int n = parse_query(queries[i], ps, 16);
        CHECK(n >= 0);
        printf("query %zu: %d pairs\n", i + 1, n);
        for (int k = 0; k < n; k++) printf("  %-8s %s%s\n", ps[k].key[0] ? ps[k].key : "(empty)", ps[k].has_eq ? "= " : "(no =)", ps[k].val);
        qsort(ps, (size_t)n, sizeof ps[0], cmp_pair);
        char canon[400];
        size_t o = 0;
        for (int k = 0; k < n; k++) {
            char e1[200], e2[200];
            pct_encode((const unsigned char *)ps[k].key, strlen(ps[k].key), e1, 1);
            pct_encode((const unsigned char *)ps[k].val, strlen(ps[k].val), e2, 1);
            o += (size_t)snprintf(canon + o, sizeof canon - o, "%s%s%s%s", k ? "&" : "", e1, ps[k].has_eq ? "=" : "", e2);
        }
        printf("  canonical: %s\n", canon);
        Pair again[16];
        int n2 = parse_query(canon, again, 16);
        CHECK(n2 == n);
        for (int k = 0; k < n; k++) CHECK(strcmp(again[k].key, ps[k].key) == 0 && strcmp(again[k].val, ps[k].val) == 0);
    }
    /* Byte fuzz: every byte string survives encode/decode in both modes. */
    long total = 0;
    for (int t = 0; t < 2000; t++) {
        unsigned char in[40], out[40];
        char e[130];
        size_t n = rnd() % 40;
        for (size_t i = 0; i < n; i++) in[i] = (unsigned char)rnd();
        for (int form = 0; form < 2; form++) {
            size_t el = pct_encode(in, n, e, form);
            CHECK(el <= 3 * n);
            long dl = pct_decode(e, el, out, form);
            CHECK(dl == (long)n && memcmp(in, out, n) == 0);
            total += (long)el;
        }
    }
    printf("fuzz round trips ok, %ld encoded characters produced\n", total);
    return 0;
}
