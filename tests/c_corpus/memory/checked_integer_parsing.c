/*
 * title: Checked decimal and hex parsing with overflow and junk detection
 * topic: memory
 * covers: strtol-style parsing without libc, range limits, error taxonomy, leading/trailing junk, INT_MIN edge, bounded scan
 * deps: libc
 */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { P_OK, P_EMPTY, P_JUNK, P_RANGE, P_NODIGITS };
static const char *pn[] = {"ok", "empty", "junk", "range", "no-digits"};

/* Parses [ws] [+-] digits, base 10 or 16 (with 0x), into [lo, hi]. `len` bounds the scan (no NUL needed). */
static int parse_i64(const char *s, size_t len, int base, int64_t lo, int64_t hi, int64_t *out) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i == len) return P_EMPTY;
    int neg = 0;
    if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; i++; }
    if (base == 16 && len - i >= 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
    size_t start = i;
    uint64_t acc = 0;
    /* accumulate the magnitude as unsigned; limit is |INT64_MIN| = 2^63 */
    const uint64_t maxmag = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    int overflow = 0;
    for (; i < len; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        if (acc > (maxmag - (uint64_t)d) / (uint64_t)base) overflow = 1;
        if (!overflow) acc = acc * (uint64_t)base + (uint64_t)d;
    }
    if (i == start) return P_NODIGITS;
    if (i != len) { while (i < len && (s[i] == ' ' || s[i] == '\t')) i++; if (i != len) return P_JUNK; }
    if (overflow) return P_RANGE;
    int64_t v = neg ? (acc == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)acc) : (int64_t)acc;
    if (v < lo || v > hi) return P_RANGE;
    *out = v;
    return P_OK;
}

int main(void) {
    struct { const char *s; int base; int64_t lo, hi; } cases[] = {
        {"0", 10, INT64_MIN, INT64_MAX},
        {"  42", 10, INT64_MIN, INT64_MAX},
        {"-17  ", 10, INT64_MIN, INT64_MAX},
        {"+5", 10, INT64_MIN, INT64_MAX},
        {"9223372036854775807", 10, INT64_MIN, INT64_MAX},
        {"9223372036854775808", 10, INT64_MIN, INT64_MAX},
        {"-9223372036854775808", 10, INT64_MIN, INT64_MAX},
        {"-9223372036854775809", 10, INT64_MIN, INT64_MAX},
        {"99999999999999999999999", 10, INT64_MIN, INT64_MAX},
        {"", 10, INT64_MIN, INT64_MAX},
        {"   ", 10, INT64_MIN, INT64_MAX},
        {"-", 10, INT64_MIN, INT64_MAX},
        {"12abc", 10, INT64_MIN, INT64_MAX},
        {"1 2", 10, INT64_MIN, INT64_MAX},
        {"0x1F", 16, INT64_MIN, INT64_MAX},
        {"ff", 16, INT64_MIN, INT64_MAX},
        {"-0xff", 16, INT64_MIN, INT64_MAX},
        {"0x", 16, INT64_MIN, INT64_MAX},
        {"7fffffffffffffff", 16, INT64_MIN, INT64_MAX},
        {"8000000000000000", 16, INT64_MIN, INT64_MAX},
        {"300", 10, 0, 255},
        {"255", 10, 0, 255},
        {"-1", 10, 0, 255},
        {"2147483648", 10, INT_MIN, INT_MAX},
        {"-2147483648", 10, INT_MIN, INT_MAX},
    };
    int nok = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t v = 0;
        int r = parse_i64(cases[i].s, strlen(cases[i].s), cases[i].base, cases[i].lo, cases[i].hi, &v);
        if (r == P_OK) { printf("%-24s -> ok %lld\n", cases[i].s, (long long)v); nok++; }
        else printf("%-24s -> %s\n", cases[i].s, pn[r]);
    }

    /* bounded scan: only the first `len` bytes are looked at, the rest is never read */
    char buf[8] = {'1', '2', '3', '4', 'x', 'y', 'z', 'w'};
    int64_t v = 0;
    int r = parse_i64(buf, 4, 10, 0, 10000, &v);
    printf("bounded 4 of \"1234xyzw\": %s %lld\n", pn[r], (long long)v);
    r = parse_i64(buf, 5, 10, 0, 10000, &v);
    printf("bounded 5 of \"1234xyzw\": %s\n", pn[r]);

    /* cross-check against arithmetic for a sweep of values, decimal and hex round trips */
    int64_t vals[] = {0, 1, -1, 12345, -98765, INT64_MAX, INT64_MIN, INT64_MAX - 1, INT64_MIN + 1};
    int rt = 0;
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        char t[40];
        snprintf(t, sizeof t, "%lld", (long long)vals[i]);
        int64_t back = 0;
        if (parse_i64(t, strlen(t), 10, INT64_MIN, INT64_MAX, &back) != P_OK || back != vals[i]) { fprintf(stderr, "rt fail %s\n", t); return 1; }
        rt++;
    }
    printf("decimal round trips: %d, accepted cases: %d\n", rt, nok);
    return 0;
}
