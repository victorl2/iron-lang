/*
 * title: Convex hull trick for DP with monotone slopes
 * topic: algorithms
 * covers: dynamic programming, convex hull trick, deque of lines, slope-monotone pointer walk, intersection comparison with cross multiplication, quadratic DP cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef long long ll;
#define N 300

typedef struct { ll m, b; } Line;

/* lines added with decreasing slope; query minimum at increasing x */
static Line hull[N + 2];
static int head, tail;

static int bad(Line l1, Line l2, Line l3) {
    /* l2 is unnecessary if intersection(l1,l3) is left of intersection(l1,l2) */
    return (l3.b - l1.b) * (l1.m - l2.m) <= (l2.b - l1.b) * (l1.m - l3.m); /* |products| < 2^42 here */
}
static void add(Line l) {
    while (tail - head >= 2 && bad(hull[tail - 2], hull[tail - 1], l)) tail--;
    hull[tail++] = l;
}
static ll eval(Line l, ll x) { return l.m * x + l.b; }
static ll query(ll x) {
    while (tail - head >= 2 && eval(hull[head + 1], x) <= eval(hull[head], x)) head++;
    return eval(hull[head], x);
}

int main(void) {
    /* Split a sequence of n items into consecutive groups, group cost = (sum)^2 + C. */
    for (int t = 0; t < 6; t++) {
        int n = 20 + rr(N - 20);
        ll C = 10 + rr(300), pre[N + 1];
        pre[0] = 0;
        for (int i = 1; i <= n; i++) pre[i] = pre[i - 1] + 1 + rr(20);
        ll slow[N + 1], fast[N + 1];
        slow[0] = fast[0] = 0;
        for (int i = 1; i <= n; i++) {
            slow[i] = -1;
            for (int j = 0; j < i; j++) {
                ll c = slow[j] + (pre[i] - pre[j]) * (pre[i] - pre[j]) + C;
                if (slow[i] < 0 || c < slow[i]) slow[i] = c;
            }
        }
        /* dp[i] = min_j dp[j] + (P_i - P_j)^2 + C = P_i^2 + C + min_j (-2 P_j * P_i + dp[j] + P_j^2) */
        head = tail = 0;
        add((Line){-2 * pre[0], 0});
        int hull_max = 1;
        for (int i = 1; i <= n; i++) {
            fast[i] = pre[i] * pre[i] + C + query(pre[i]);
            add((Line){-2 * pre[i], fast[i] + pre[i] * pre[i]});
            if (tail - head > hull_max) hull_max = tail - head;
        }
        for (int i = 0; i <= n; i++) CHECK(slow[i] == fast[i]);
        printf("case %d: n=%d C=%lld total=%lld hull_peak=%d\n", t, n, C, fast[n], hull_max);
    }
    return 0;
}
