/*
 * title: Run-length encoding: pairs and PackBits
 * topic: algorithms
 * covers: run-length encoding, PackBits literals and repeats, run splitting at 255, malformed input rejection, dynamic buffers
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

/* Run-length encoding in two byte formats plus a bit-packed variant. */
typedef struct {
    unsigned char *data;
    size_t len;
} Buf;

static void put(Buf *b, unsigned char c) {
    b->data = realloc(b->data, b->len + 1);
    b->data[b->len++] = c;
}

/* Format A: (count, byte) pairs, count 1..255. */
static void rle_pairs(const unsigned char *s, size_t n, Buf *out) {
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && s[j] == s[i] && j - i < 255)
            j++;
        put(out, (unsigned char)(j - i));
        put(out, s[i]);
        i = j;
    }
}

static int unrle_pairs(const unsigned char *e, size_t n, Buf *out) {
    if (n % 2)
        return -1;
    for (size_t i = 0; i < n; i += 2) {
        if (e[i] == 0)
            return -1;
        for (int k = 0; k < e[i]; k++)
            put(out, e[i + 1]);
    }
    return 0;
}

/* Format B (PackBits style): control byte c: 0..127 => c+1 literals follow,
 * 129..255 => repeat next byte 257-c times. 128 is reserved. */
static void packbits(const unsigned char *s, size_t n, Buf *out) {
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && s[i + run] == s[i] && run < 128)
            run++;
        if (run >= 3) {
            put(out, (unsigned char)(257 - run));
            put(out, s[i]);
            i += run;
        } else {
            size_t start = i, cnt = 0;
            while (i < n && cnt < 128) {
                size_t r = 1;
                while (i + r < n && s[i + r] == s[i] && r < 3)
                    r++;
                if (r >= 3)
                    break;
                i++;
                cnt++;
            }
            put(out, (unsigned char)(cnt - 1));
            for (size_t k = 0; k < cnt; k++)
                put(out, s[start + k]);
        }
    }
}

static int unpackbits(const unsigned char *e, size_t n, Buf *out) {
    size_t i = 0;
    while (i < n) {
        unsigned c = e[i++];
        if (c < 128) {
            if (i + c + 1 > n)
                return -1;
            for (unsigned k = 0; k <= c; k++)
                put(out, e[i++]);
        } else if (c > 128) {
            if (i >= n)
                return -1;
            for (unsigned k = 0; k < 257 - c; k++)
                put(out, e[i]);
            i++;
        } else
            return -1;
    }
    return 0;
}

static void fill(unsigned char *s, size_t n, int mode) {
    size_t i = 0;
    while (i < n) {
        size_t run = mode == 0 ? 1 + rnd() % 4 : mode == 1 ? 1 + rnd() % 300 : 1 + rnd() % 2;
        unsigned char c = (unsigned char)(rnd() % (mode == 2 ? 256 : 4));
        for (size_t k = 0; k < run && i < n; k++)
            s[i++] = c;
    }
}

int main(void) {
    const char *demo = "WWWWWWWWWWWWBWWWWWWWWWWWWBBBWWWWWWWWWWWWWWWWWWWWWWWWBWWWWWWWWWWWWWW";
    Buf e = {NULL, 0}, d = {NULL, 0};
    rle_pairs((const unsigned char *)demo, strlen(demo), &e);
    printf("demo %zu bytes -> %zu bytes:", strlen(demo), e.len);
    for (size_t i = 0; i < e.len; i += 2)
        printf(" %d%c", e.data[i], e.data[i + 1]);
    printf("\n");
    check(unrle_pairs(e.data, e.len, &d) == 0 && d.len == strlen(demo) && memcmp(d.data, demo, d.len) == 0,
          "demo roundtrip");
    free(e.data);
    free(d.data);

    static unsigned char src[6000];
    const char *names[] = {"short runs", "long runs (over 255)", "noisy"};
    for (int mode = 0; mode < 3; mode++) {
        size_t n = 5000;
        fill(src, n, mode);
        Buf a = {NULL, 0}, b = {NULL, 0}, ra = {NULL, 0}, rb = {NULL, 0};
        rle_pairs(src, n, &a);
        packbits(src, n, &b);
        check(unrle_pairs(a.data, a.len, &ra) == 0 && ra.len == n && memcmp(ra.data, src, n) == 0,
              "pairs roundtrip");
        check(unpackbits(b.data, b.len, &rb) == 0 && rb.len == n && memcmp(rb.data, src, n) == 0,
              "packbits roundtrip");
        printf("%-22s n=%zu pairs=%zu packbits=%zu\n", names[mode], n, a.len, b.len);
        free(a.data);
        free(b.data);
        free(ra.data);
        free(rb.data);
    }
    /* malformed streams are rejected */
    Buf junk = {NULL, 0};
    unsigned char bad1[] = {3, 'a', 0};
    unsigned char bad2[] = {0, 'x'};
    unsigned char bad3[] = {128};
    unsigned char bad4[] = {5, 'a', 'b'};
    check(unrle_pairs(bad1, 3, &junk) < 0, "odd length");
    check(unrle_pairs(bad2, 2, &junk) < 0, "zero count");
    check(unpackbits(bad3, 1, &junk) < 0, "reserved control");
    check(unpackbits(bad4, 3, &junk) < 0, "truncated literals");
    printf("malformed inputs rejected: 4\n");
    free(junk.data);
    return 0;
}
