/*
 * title: Strict UTF-8 encoder and validator
 * topic: algorithms
 * covers: UTF-8 encoding, bit manipulation, overlong and surrogate rejection, exhaustive scalar roundtrip, error enums
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* UTF-8 encoder, strict decoder and validator (rejects overlongs, surrogates, > U+10FFFF). */
typedef enum { OK, TRUNCATED, BAD_LEAD, BAD_CONT, OVERLONG, SURROGATE, TOO_BIG } Status;

static const char *status_name(Status s) {
    static const char *names[] = {"ok", "truncated", "bad-lead", "bad-continuation", "overlong",
                                  "surrogate", "out-of-range"};
    return names[s];
}

static int encode(unsigned cp, unsigned char *out) {
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | cp >> 6);
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | cp >> 12);
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | cp >> 18);
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

static Status decode(const unsigned char *s, size_t n, size_t *used, unsigned *cp) {
    unsigned c = s[0];
    int len;
    unsigned v, min;
    if (c < 0x80) {
        *used = 1;
        *cp = c;
        return OK;
    } else if (c >= 0xC0 && c < 0xE0) {
        len = 2;
        v = c & 0x1F;
        min = 0x80;
    } else if (c >= 0xE0 && c < 0xF0) {
        len = 3;
        v = c & 0x0F;
        min = 0x800;
    } else if (c >= 0xF0 && c < 0xF8) {
        len = 4;
        v = c & 0x07;
        min = 0x10000;
    } else
        return BAD_LEAD;
    if (n < (size_t)len) {
        /* still report a bad continuation if one is visible */
        for (size_t i = 1; i < n; i++)
            if ((s[i] & 0xC0) != 0x80)
                return BAD_CONT;
        return TRUNCATED;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return BAD_CONT;
        v = v << 6 | (s[i] & 0x3F);
    }
    if (v < min)
        return OVERLONG;
    if (v >= 0xD800 && v <= 0xDFFF)
        return SURROGATE;
    if (v > 0x10FFFF)
        return TOO_BIG;
    *used = (size_t)len;
    *cp = v;
    return OK;
}

/* returns number of code points or -1; sets *status and *at */
static long validate(const unsigned char *s, size_t n, Status *status, size_t *at) {
    size_t i = 0;
    long count = 0;
    while (i < n) {
        size_t used = 0;
        unsigned cp;
        Status st = decode(s + i, n - i, &used, &cp);
        if (st != OK) {
            *status = st;
            *at = i;
            return -1;
        }
        i += used;
        count++;
    }
    *status = OK;
    return count;
}

int main(void) {
    unsigned samples[] = {0x24, 0x7F, 0x80, 0xA2, 0x7FF, 0x800, 0x20AC, 0xD7FF, 0xE000, 0xFFFF,
                          0x10000, 0x1F600, 0x10FFFF};
    unsigned char buf[8];
    printf("encodings:\n");
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        int l = encode(samples[i], buf);
        size_t used;
        unsigned back;
        check(decode(buf, (size_t)l, &used, &back) == OK && back == samples[i] && used == (size_t)l,
              "roundtrip");
        printf("  U+%04X:", samples[i]);
        for (int k = 0; k < l; k++)
            printf(" %02X", buf[k]);
        printf("\n");
    }
    /* exhaustive roundtrip over every valid scalar value */
    long ok = 0, total_bytes = 0;
    for (unsigned cp = 0; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        int l = encode(cp, buf);
        size_t used;
        unsigned back;
        check(decode(buf, (size_t)l, &used, &back) == OK && back == cp, "exhaustive");
        ok++;
        total_bytes += l;
    }
    printf("exhaustive: %ld scalar values, %ld bytes\n", ok, total_bytes);
    check(ok == 1112064, "scalar count");

    struct {
        const char *name;
        unsigned char bytes[6];
        size_t len;
    } bad[] = {
        {"overlong slash", {0xC0, 0xAF}, 2},
        {"overlong 3-byte", {0xE0, 0x80, 0x80}, 3},
        {"surrogate D800", {0xED, 0xA0, 0x80}, 3},
        {"above 10FFFF", {0xF4, 0x90, 0x80, 0x80}, 4},
        {"lone continuation", {0x80}, 1},
        {"truncated euro", {0xE2, 0x82}, 2},
        {"bad continuation", {0xE2, 0x28, 0xA1}, 3},
        {"F8 lead", {0xF8, 0x88, 0x80, 0x80, 0x80}, 5},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Status st;
        size_t at;
        long r = validate(bad[i].bytes, bad[i].len, &st, &at);
        check(r < 0, "must reject");
        printf("  %-18s -> %s at %zu\n", bad[i].name, status_name(st), at);
    }
    /* random byte soup: validator agrees with re-encoding of what it accepts */
    long accepted = 0, rejected = 0;
    for (int t = 0; t < 4000; t++) {
        unsigned char b[6];
        size_t n = 1 + rnd() % 4;
        for (size_t i = 0; i < n; i++)
            b[i] = (unsigned char)(rnd() % 3 == 0 ? rnd() % 256 : 0x80 + rnd() % 64);
        if (rnd() % 2)
            b[0] = (unsigned char)(0xC0 + rnd() % 0x30);
        Status st;
        size_t at;
        long r = validate(b, n, &st, &at);
        if (r >= 0) {
            unsigned char re[32];
            size_t k = 0, i = 0;
            while (i < n) {
                size_t used;
                unsigned cp;
                check(decode(b + i, n - i, &used, &cp) == OK, "second pass");
                k += (size_t)encode(cp, re + k);
                i += used;
            }
            check(k == n && memcmp(re, b, n) == 0, "canonical re-encode");
            accepted++;
        } else
            rejected++;
    }
    printf("random bytes: %ld accepted, %ld rejected\n", accepted, rejected);
    return 0;
}
