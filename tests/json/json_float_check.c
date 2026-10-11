/* json_float_check.c -- the json module's float formatter (Ryu, in
 * iron_json.c) against a brute-force shortest round-trip search, over random
 * bit patterns, decimal-looking values and subnormals.
 * usage: json_float_check [count] [seed] */
#include "stdlib/iron_json.c"
#include <time.h>
/* Brute-force reference: the first precision whose correctly rounded form
 * reads back (the correctly rounded form is the closest of its length). */
static int json_shortest_slow(double v, char *digits, int *exp, int *neg) {
    char sci[40];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(sci, sizeof(sci), "%.*e", prec - 1, v);
        if (prec == 17 || strtod(sci, NULL) == v) break;
    }
    const char *c = sci;
    *neg = (*c == '-');
    if (*neg) c++;
    int n = 0;
    for (; *c && *c != 'e'; c++) {
        if (*c >= '0' && *c <= '9') digits[n++] = *c;
    }
    *exp = (*c == 'e') ? atoi(c + 1) : 0;
    while (n > 1 && digits[n - 1] == '0') n--;
    digits[n] = '\0';
    return n;
}
static uint64_t rng = 88172645463325252ULL;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
int main(int argc, char **argv) {
    long n = argc > 1 ? atol(argv[1]) : 1000000;
    if (argc > 2) rng = strtoull(argv[2], NULL, 10);
    long bad = 0;
    for (long i = 0; i < n; i++) {
        double v; uint64_t b;
        int mode = (int)(i % 4);
        if (mode == 0) { b = next(); memcpy(&v, &b, 8); }
        else if (mode == 1) { v = (double)(int64_t)(next() % 2000000001) / 1000.0 * ((next() & 1) ? 1 : -1); }
        else if (mode == 2) { b = next() & 0x000FFFFFFFFFFFFFULL; memcpy(&v, &b, 8); } /* subnormals */
        else { v = (double)(next() % 1000000) * pow(10.0, (double)((int)(next() % 40) - 20)); }
        if (!isfinite(v) || v == 0.0) continue;
        char d1[32], d2[32]; int e1, e2, n1, n2;
        json_shortest(v, d1, &e1, &n1);
        json_shortest_slow(v, d2, &e2, &n2);
        if (strcmp(d1, d2) != 0 || e1 != e2 || n1 != n2) {
            if (bad < 10) printf("MISMATCH %.17g ryu=%s e%d slow=%s e%d\n", v, d1, e1, d2, e2);
            bad++;
        }
    }
    printf("checked %ld, mismatches %ld\n", n, bad);
    return bad != 0;
}
