/*
 * title: Smallest and largest number after removing digits
 * topic: algorithms
 * covers: monotonic stack, greedy deletion, leading zeros, lexicographic subsequences, brute force over subsets
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 3141592u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Remove exactly k digits to minimise the numeric value; strip leading zeros. */
static void smallest_after_removing(const char *num, int k, char *out) {
    int n = (int)strlen(num);
    char stack[64];
    int top = 0;
    for (int i = 0; i < n; i++) {
        while (k > 0 && top > 0 && stack[top - 1] > num[i]) {
            top--;
            k--;
        }
        stack[top++] = num[i];
    }
    top -= k; /* still have deletions left: drop from the tail */
    int start = 0;
    while (start < top - 1 && stack[start] == '0')
        start++;
    if (top <= 0) {
        strcpy(out, "0");
        return;
    }
    memcpy(out, stack + start, (size_t)(top - start));
    out[top - start] = '\0';
}

/* Keep exactly `keep` digits forming the lexicographically largest subsequence. */
static void largest_keeping(const char *num, int keep, char *out) {
    int n = (int)strlen(num), k = n - keep, top = 0;
    char stack[64];
    for (int i = 0; i < n; i++) {
        while (k > 0 && top > 0 && stack[top - 1] < num[i]) {
            top--;
            k--;
        }
        stack[top++] = num[i];
    }
    memcpy(out, stack, (size_t)keep);
    out[keep] = '\0';
}

static unsigned long long value_of(const char *s) {
    unsigned long long v = 0;
    for (; *s; s++)
        v = v * 10 + (unsigned)(*s - '0');
    return v;
}

static unsigned long long brute_min(const char *num, int k) {
    int n = (int)strlen(num);
    unsigned long long best = ~0ULL;
    for (unsigned m = 0; m < (1u << n); m++) {
        int c = 0;
        for (int i = 0; i < n; i++)
            c += (int)(m >> i & 1u);
        if (c != n - k)
            continue;
        char buf[32];
        int len = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1u)
                buf[len++] = num[i];
        buf[len] = '\0';
        unsigned long long v = value_of(buf);
        if (v < best)
            best = v;
    }
    return best;
}

static void brute_max_str(const char *num, int keep, char *out) {
    int n = (int)strlen(num);
    out[0] = '\0';
    for (unsigned m = 0; m < (1u << n); m++) {
        int c = 0;
        for (int i = 0; i < n; i++)
            c += (int)(m >> i & 1u);
        if (c != keep)
            continue;
        char buf[32];
        int len = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1u)
                buf[len++] = num[i];
        buf[len] = '\0';
        if (strcmp(buf, out) > 0)
            strcpy(out, buf);
    }
}

int main(void) {
    struct {
        const char *num;
        int k;
    } ex[] = {{"1432219", 3}, {"10200", 1}, {"10", 2}, {"112", 1}, {"9", 1}, {"1234567890", 9},
              {"100200300", 4}, {"54321", 2}, {"1111111", 3}};
    char out[64];
    for (int i = 0; i < 9; i++) {
        smallest_after_removing(ex[i].num, ex[i].k, out);
        printf("remove %d from %-11s -> %s\n", ex[i].k, ex[i].num, out);
    }
    smallest_after_removing("1432219", 3, out);
    check(strcmp(out, "1219") == 0, "classic example");
    smallest_after_removing("10200", 1, out);
    check(strcmp(out, "200") == 0, "leading zero stripped");
    long sum_small = 0, sum_large = 0;
    for (int t = 0; t < 400; t++) {
        int n = 1 + (int)(rnd() % 11);
        char num[16];
        for (int i = 0; i < n; i++)
            num[i] = (char)('0' + (rnd() % 3 == 0 ? 0 : rnd() % 10));
        num[n] = '\0';
        int k = (int)(rnd() % (unsigned)(n + 1));
        smallest_after_removing(num, k, out);
        unsigned long long got = value_of(out);
        unsigned long long want = k == n ? 0 : brute_min(num, k);
        check(got == want, "smallest matches brute force");
        check(out[0] != '0' || out[1] == '\0', "no leading zeros");
        sum_small += (long)(got % 1000);
        if (k < n) {
            char big[16], bb[16];
            largest_keeping(num, n - k, big);
            brute_max_str(num, n - k, bb);
            check(strcmp(big, bb) == 0, "largest subsequence matches brute force");
            sum_large += (long)(value_of(big) % 1000);
        }
    }
    printf("400 random strings verified: checksum small=%ld large=%ld\n", sum_small, sum_large);
    largest_keeping("9173", 2, out);
    printf("largest 2-digit subsequence of 9173: %s\n", out);
    largest_keeping("3141592653589793", 6, out);
    printf("largest 6-digit subsequence of pi digits: %s\n", out);
    return 0;
}
