/*
 * title: Punycode encoder and decoder with the RFC 3492 sample strings
 * topic: networking
 * covers: bootstring adaptation, generalized variable-length integers, delimiter handling, case-insensitive digits, overflow and malformed input rejection, UTF-8 to code point conversion, xn-- label wrapping
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { BASE = 36, TMIN = 1, TMAX = 26, SKEW = 38, DAMP = 700, INITIAL_BIAS = 72, INITIAL_N = 128 };

static uint32_t adapt(uint32_t delta, uint32_t numpoints, int first) {
    delta = first ? delta / DAMP : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((BASE - TMIN) * TMAX) / 2) { delta /= BASE - TMIN; k += BASE; }
    return k + (BASE - TMIN + 1) * delta / (delta + SKEW);
}

static char digit_char(uint32_t d) { return (char)(d < 26 ? 'a' + d : '0' + (d - 26)); }
static int char_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0' + 26;
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return c - 'A';
    return -1;
}

/* returns output length or -1 on overflow / capacity */
static long puny_encode(const uint32_t *in, size_t n, char *out, size_t cap) {
    size_t o = 0, b = 0;
    for (size_t i = 0; i < n; i++) if (in[i] < 128) { if (o + 1 >= cap) return -1; out[o++] = (char)in[i]; b++; }
    size_t h = b;
    if (b) { if (o + 1 >= cap) return -1; out[o++] = '-'; }
    uint32_t nn = INITIAL_N, delta = 0, bias = INITIAL_BIAS;
    while (h < n) {
        uint32_t m = UINT32_MAX;
        for (size_t i = 0; i < n; i++) if (in[i] >= nn && in[i] < m) m = in[i];
        if ((m - nn) > (UINT32_MAX - delta) / (uint32_t)(h + 1)) return -1;
        delta += (m - nn) * (uint32_t)(h + 1);
        nn = m;
        for (size_t i = 0; i < n; i++) {
            if (in[i] < nn) { if (++delta == 0) return -1; }
            if (in[i] == nn) {
                uint32_t q = delta;
                for (uint32_t k = BASE;; k += BASE) {
                    uint32_t t = k <= bias ? TMIN : (k >= bias + TMAX ? TMAX : k - bias);
                    if (q < t) break;
                    if (o + 1 >= cap) return -1;
                    out[o++] = digit_char(t + (q - t) % (BASE - t));
                    q = (q - t) / (BASE - t);
                }
                if (o + 1 >= cap) return -1;
                out[o++] = digit_char(q);
                bias = adapt(delta, (uint32_t)(h + 1), h == b);
                delta = 0;
                h++;
            }
        }
        delta++; nn++;
    }
    out[o] = 0;
    return (long)o;
}

typedef enum { P_OK, P_BAD_DIGIT, P_OVERFLOW, P_TRUNCATED, P_NONBASIC_IN_BASIC, P_CAP } PErr;
static const char *pname[] = { "ok", "bad-digit", "overflow", "truncated", "non-basic-before-delimiter", "capacity" };

static PErr puny_decode(const char *in, uint32_t *out, size_t cap, size_t *outn) {
    size_t n = 0;
    const char *dl = strrchr(in, '-');
    size_t b = dl ? (size_t)(dl - in) : 0;
    for (size_t j = 0; j < b; j++) {
        if ((unsigned char)in[j] >= 128) return P_NONBASIC_IN_BASIC;
        if (n >= cap) return P_CAP;
        out[n++] = (unsigned char)in[j];
    }
    uint32_t nn = INITIAL_N, i = 0, bias = INITIAL_BIAS;
    size_t p = b ? b + 1 : 0;
    size_t len = strlen(in);
    while (p < len) {
        uint32_t oldi = i, w = 1;
        for (uint32_t k = BASE;; k += BASE) {
            if (p >= len) return P_TRUNCATED;
            int d = char_digit((unsigned char)in[p++]);
            if (d < 0) return P_BAD_DIGIT;
            if ((uint32_t)d > (UINT32_MAX - i) / w) return P_OVERFLOW;
            i += (uint32_t)d * w;
            uint32_t t = k <= bias ? TMIN : (k >= bias + TMAX ? TMAX : k - bias);
            if ((uint32_t)d < t) break;
            if (w > UINT32_MAX / (BASE - t)) return P_OVERFLOW;
            w *= BASE - t;
        }
        bias = adapt(i - oldi, (uint32_t)(n + 1), oldi == 0);
        if (i / (uint32_t)(n + 1) > UINT32_MAX - nn) return P_OVERFLOW;
        nn += i / (uint32_t)(n + 1);
        i %= (uint32_t)(n + 1);
        if (nn > 0x10ffff || (nn >= 0xd800 && nn <= 0xdfff)) return P_OVERFLOW;
        if (n >= cap) return P_CAP;
        memmove(out + i + 1, out + i, (n - i) * sizeof *out);
        out[i++] = nn;
        n++;
    }
    *outn = n;
    return P_OK;
}

static size_t utf8_to_cps(const char *s, uint32_t *o) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p;
        int k = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        c = k == 0 ? c : (c & (0x3f >> k));
        for (int i = 0; i < k; i++) c = (c << 6) | (p[1 + i] & 0x3f);
        p += k + 1;
        o[n++] = c;
    }
    return n;
}
static size_t cps_to_utf8(const uint32_t *c, size_t n, char *o) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = c[i];
        if (v < 0x80) o[k++] = (char)v;
        else if (v < 0x800) { o[k++] = (char)(0xc0 | (v >> 6)); o[k++] = (char)(0x80 | (v & 63)); }
        else if (v < 0x10000) { o[k++] = (char)(0xe0 | (v >> 12)); o[k++] = (char)(0x80 | ((v >> 6) & 63)); o[k++] = (char)(0x80 | (v & 63)); }
        else { o[k++] = (char)(0xf0 | (v >> 18)); o[k++] = (char)(0x80 | ((v >> 12) & 63)); o[k++] = (char)(0x80 | ((v >> 6) & 63)); o[k++] = (char)(0x80 | (v & 63)); }
    }
    o[k] = 0;
    return k;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const struct { const char *label; uint32_t cp[40]; size_t n; const char *puny; } samples[] = {
    { "Arabic (Egyptian)", { 0x644, 0x64A, 0x647, 0x645, 0x627, 0x628, 0x62A, 0x643, 0x644, 0x645, 0x648, 0x634, 0x639, 0x631, 0x628, 0x64A, 0x61F }, 17, "egbpdaj6bu4bxfgehfvwxn" },
    { "Chinese (simplified)", { 0x4ED6, 0x4EEC, 0x4E3A, 0x4EC0, 0x4E48, 0x4E0D, 0x8BF4, 0x4E2D, 0x6587 }, 9, "ihqwcrb4cv8a8dqg056pqjye" },
    { "Chinese (traditional)", { 0x4ED6, 0x5011, 0x7232, 0x4EC0, 0x9EBD, 0x4E0D, 0x8AAA, 0x4E2D, 0x6587 }, 9, "ihqwctvzc91f659drss3x8bo0yb" },
    { "Czech", { 0x50, 0x72, 0x6F, 0x10D, 0x70, 0x72, 0x6F, 0x73, 0x74, 0x11B, 0x6E, 0x65, 0x6D, 0x6C, 0x75, 0x76, 0xED, 0x10D, 0x65, 0x73, 0x6B, 0x79 }, 22, "Proprostnemluvesky-uyb24dma41a" },
    { "Hebrew", { 0x5DC, 0x5DE, 0x5D4, 0x5D4, 0x5DD, 0x5E4, 0x5E9, 0x5D5, 0x5D8, 0x5DC, 0x5D0, 0x5DE, 0x5D3, 0x5D1, 0x5E8, 0x5D9, 0x5DD, 0x5E2, 0x5D1, 0x5E8, 0x5D9, 0x5EA }, 22, "4dbcagdahymbxekheh6e0a7fei0b" },
    { "Hindi", { 0x92F, 0x939, 0x932, 0x94B, 0x917, 0x939, 0x93F, 0x928, 0x94D, 0x926, 0x940, 0x915, 0x94D, 0x92F, 0x94B, 0x902, 0x928, 0x939, 0x940, 0x902, 0x92C, 0x94B, 0x932, 0x938, 0x915, 0x924, 0x947, 0x939, 0x948, 0x902 }, 30, "i1baa7eci9glrd9b2ae1bj0hfcgg6iyaf8o0a1dig0cd" },
    { "Japanese kanji+hiragana", { 0x306A, 0x305C, 0x307F, 0x3093, 0x306A, 0x65E5, 0x672C, 0x8A9E, 0x3092, 0x8A71, 0x3057, 0x3066, 0x304F, 0x308C, 0x306A, 0x3044, 0x306E, 0x304B }, 18, "n8jok5ay5dzabd5bym9f0cm5685rrjetr6pdxa" },
    { "Korean", { 0xC138, 0xACC4, 0xC758, 0xBAA8, 0xB4E0, 0xC0AC, 0xB78C, 0xB4E4, 0xC774, 0xD55C, 0xAD6D, 0xC5B4, 0xB97C, 0xC774, 0xD574, 0xD55C, 0xB2E4, 0xBA74, 0xC5BC, 0xB9C8, 0xB098, 0xC88B, 0xC744, 0xAE4C }, 24, "989aomsvi5e83db1d2a355cv1e0vak1dwrv93d5xbh15a0dt30a5jpsd879ccm6fea98c" },
    { "Russian", { 0x43F, 0x43E, 0x447, 0x435, 0x43C, 0x443, 0x436, 0x435, 0x43E, 0x43D, 0x438, 0x43D, 0x435, 0x433, 0x43E, 0x432, 0x43E, 0x440, 0x44F, 0x442, 0x43F, 0x43E, 0x440, 0x443, 0x441, 0x441, 0x43A, 0x438 }, 28, "b1abfaaepdrnnbgefbadotcwatmq2g4l" },
    { "Spanish", { 0x50, 0x6F, 0x72, 0x71, 0x75, 0xE9, 0x6E, 0x6F, 0x70, 0x75, 0x65, 0x64, 0x65, 0x6E, 0x73, 0x69, 0x6D, 0x70, 0x6C, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x65, 0x68, 0x61, 0x62, 0x6C, 0x61, 0x72, 0x65, 0x6E, 0x45, 0x73, 0x70, 0x61, 0xF1, 0x6F, 0x6C }, 40, "PorqunopuedensimplementehablarenEspaol-fmd56a" },
    { "Vietnamese", { 0x54, 0x1EA1, 0x69, 0x73, 0x61, 0x6F, 0x68, 0x1ECD, 0x6B, 0x68, 0xF4, 0x6E, 0x67, 0x74, 0x68, 0x1EC3, 0x63, 0x68, 0x1EC9, 0x6E, 0xF3, 0x69, 0x74, 0x69, 0x1EBF, 0x6E, 0x67, 0x56, 0x69, 0x1EC7, 0x74 }, 31, "TisaohkhngthchnitingVit-kjcr8268qyxafd2f1b9g" },
    { "3nen B-gumi", { 0x33, 0x5E74, 0x42, 0x7D44, 0x91D1, 0x516B, 0x5148, 0x751F }, 8, "3B-ww4c5e180e575a65lsy2b" },
    { "amuro namie", { 0x5B89, 0x5BA4, 0x5948, 0x7F8E, 0x6075, 0x2D, 0x77, 0x69, 0x74, 0x68, 0x2D, 0x53, 0x55, 0x50, 0x45, 0x52, 0x2D, 0x4D, 0x4F, 0x4E, 0x4B, 0x45, 0x59, 0x53 }, 24, "-with-SUPER-MONKEYS-pc58ag80a8qai00g7n9n" },
    { "Hello-Another-Way", { 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x2D, 0x41, 0x6E, 0x6F, 0x74, 0x68, 0x65, 0x72, 0x2D, 0x57, 0x61, 0x79, 0x2D, 0x305D, 0x308C, 0x305E, 0x308C, 0x306E, 0x5834, 0x6240 }, 25, "Hello-Another-Way--fc4qua05auwb3674vfr0b" },
    { "hitotsu yane no shita 2", { 0x3072, 0x3068, 0x3064, 0x5C4B, 0x6839, 0x306E, 0x4E0B, 0x32 }, 8, "2-u9tlzr9756bt3uc0v" },
    { "Maji de Koi suru 5 byou mae", { 0x4D, 0x61, 0x6A, 0x69, 0x3067, 0x4B, 0x6F, 0x69, 0x3059, 0x308B, 0x35, 0x79D2, 0x524D }, 13, "MajiKoi5-783gue6qz075azm5e" },
    { "pafii de runba", { 0x30D1, 0x30D5, 0x30A3, 0x30FC, 0x64, 0x65, 0x30EB, 0x30F3, 0x30D0 }, 9, "de-jg4avhby1noc0d" },
    { "sono supiido de", { 0x305D, 0x306E, 0x30B9, 0x30D4, 0x30FC, 0x30C9, 0x3067 }, 7, "d9juau41awczczp" },
    { "ASCII with delimiter", { 0x2D, 0x3E, 0x20, 0x24, 0x31, 0x2E, 0x30, 0x30, 0x20, 0x3C, 0x2D }, 11, "-> $1.00 <--" },
};

static uint32_t rs = 0x70756e79u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    int ns = (int)(sizeof samples / sizeof samples[0]);
    for (int i = 0; i < ns; i++) {
        char out[200];
        long l = puny_encode(samples[i].cp, samples[i].n, out, sizeof out);
        CHECK(l > 0);
        /* basic part must match exactly; extended digits are compared case-insensitively (the RFC mixes case annotations) */
        CHECK(ieq(out, samples[i].puny));
        const char *dl = strrchr(samples[i].puny, '-');
        size_t basic = 0;
        for (size_t k = 0; k < samples[i].n; k++) if (samples[i].cp[k] < 128) basic++;
        CHECK(!dl || strncmp(out, samples[i].puny, basic) == 0);
        uint32_t back[100];
        size_t bn;
        CHECK(puny_decode(samples[i].puny, back, 100, &bn) == P_OK);
        CHECK(bn == samples[i].n && memcmp(back, samples[i].cp, bn * sizeof back[0]) == 0);
        char u8[300];
        size_t ul = cps_to_utf8(samples[i].cp, samples[i].n, u8);
        uint32_t again[100];
        CHECK(utf8_to_cps(u8, again) == samples[i].n && memcmp(again, samples[i].cp, samples[i].n * sizeof again[0]) == 0);
        printf("(%c) %-28s %2zu cps %3zu utf8 bytes -> %s\n", 'A' + i, samples[i].label, samples[i].n, ul, out);
    }
    /* Domain labels. */
    static const char *doms[] = { "b\303\274cher", "m\303\274nchen", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "plain", "\xf0\x9f\x98\x80" };
    for (size_t i = 0; i < sizeof doms / sizeof doms[0]; i++) {
        uint32_t cp[64];
        size_t n = utf8_to_cps(doms[i], cp);
        int ascii = 1;
        for (size_t k = 0; k < n; k++) if (cp[k] >= 128) ascii = 0;
        char lab[100];
        if (ascii) snprintf(lab, sizeof lab, "%s", doms[i]);
        else { char p[90]; CHECK(puny_encode(cp, n, p, sizeof p) > 0); snprintf(lab, sizeof lab, "xn--%s", p); }
        printf("label %-12s -> %s\n", doms[i], lab);
        if (!ascii) {
            uint32_t back[64]; size_t bn;
            CHECK(puny_decode(lab + 4, back, 64, &bn) == P_OK && bn == n && memcmp(back, cp, n * sizeof *cp) == 0);
        }
    }
    /* Malformed decodes. */
    static const char *bad[] = { "ab-\xc3\xa9", "abc!", "a-9", "99999999999", "u", "de-jg4avhby1noc0" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uint32_t o[64]; size_t n;
        PErr e = puny_decode(bad[i], o, 64, &n);
        printf("decode %-20s -> %s\n", bad[i], pname[e]);
    }
    /* Random round trips over mixed ranges. */
    long cps = 0, chars = 0;
    for (int t = 0; t < 3000; t++) {
        uint32_t cp[30];
        size_t n = 1 + rnd() % 30;
        for (size_t i = 0; i < n; i++) {
            uint32_t r = rnd();
            uint32_t kind = r % 5;
            uint32_t v = rnd();
            cp[i] = kind == 0 ? 'a' + v % 26 : kind == 1 ? 0xa0 + v % 0x300 : kind == 2 ? 0x4e00 + v % 0x5000 : kind == 3 ? 0x1f300 + v % 0x300 : (kind == 4 ? '-' : 'x');
        }
        char out[400];
        long l = puny_encode(cp, n, out, sizeof out);
        CHECK(l > 0);
        uint32_t back[64]; size_t bn;
        CHECK(puny_decode(out, back, 64, &bn) == P_OK && bn == n && memcmp(back, cp, n * sizeof *cp) == 0);
        cps += (long)n; chars += l;
    }
    printf("3000 random strings round-trip: %ld code points -> %ld characters\n", cps, chars);
    return 0;
}
