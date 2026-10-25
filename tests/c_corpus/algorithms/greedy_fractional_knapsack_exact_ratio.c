/*
 * title: Fractional knapsack with exact rational arithmetic
 * topic: algorithms
 * covers: value density greedy, cross-multiplication comparison, rational totals, LP upper bound, 0/1 DP comparison
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int id;
    long long w, v;
} Item;

typedef struct {
    long long num, den; /* den > 0, reduced */
} Frac;

static unsigned st = 24680u;

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

static long long gcdll(long long a, long long b) {
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

static Frac mk(long long n, long long d) {
    long long g = gcdll(n, d);
    if (g == 0)
        g = 1;
    return (Frac){n / g, d / g};
}

static Frac add(Frac a, Frac b) {
    return mk(a.num * b.den + b.num * a.den, a.den * b.den);
}

/* density order: v_a / w_a > v_b / w_b  <=>  v_a * w_b > v_b * w_a */
static int by_density(const void *pa, const void *pb) {
    const Item *a = pa, *b = pb;
    long long l = a->v * b->w, r = b->v * a->w;
    if (l != r)
        return l > r ? -1 : 1;
    if (a->w != b->w)
        return a->w < b->w ? -1 : 1;
    return a->id - b->id;
}

/* Fills take[id] with the fraction taken as (num,den); returns total value. */
static Frac fractional(Item *it, int n, long long cap, Frac *take) {
    qsort(it, (size_t)n, sizeof(Item), by_density);
    Frac total = {0, 1};
    for (int i = 0; i < n; i++)
        take[it[i].id] = (Frac){0, 1};
    for (int i = 0; i < n && cap > 0; i++) {
        if (it[i].w <= cap) {
            cap -= it[i].w;
            take[it[i].id] = (Frac){1, 1};
            total = add(total, (Frac){it[i].v, 1});
        } else {
            take[it[i].id] = mk(cap, it[i].w);
            total = add(total, mk(it[i].v * cap, it[i].w));
            cap = 0;
        }
    }
    return total;
}

static long long knap01(const Item *it, int n, long long cap) {
    long long *dp = calloc((size_t)cap + 1, sizeof(long long));
    for (int i = 0; i < n; i++)
        for (long long c = cap; c >= it[i].w; c--)
            if (dp[c - it[i].w] + it[i].v > dp[c])
                dp[c] = dp[c - it[i].w] + it[i].v;
    long long r = dp[cap];
    free(dp);
    return r;
}

static void show(Frac f) {
    if (f.den == 1)
        printf("%lld", f.num);
    else
        printf("%lld/%lld", f.num, f.den);
}

int main(void) {
    Item classic[] = {{0, 10, 60}, {1, 20, 100}, {2, 30, 120}};
    Frac take[8];
    Frac t = fractional(classic, 3, 50, take);
    printf("classic (cap 50): value=");
    show(t);
    printf(" takes:");
    for (int i = 0; i < 3; i++) {
        printf(" i%d=", i);
        show(take[i]);
    }
    printf("\n");
    check(t.num == 240 && t.den == 1, "classic value 240");
    Item odd[] = {{0, 3, 10}, {1, 7, 20}, {2, 5, 9}};
    t = fractional(odd, 3, 11, take);
    printf("odd weights (cap 11): value=");
    show(t);
    printf("\n");
    for (int trial = 0; trial < 6; trial++) {
        int n = 5 + trial * 6;
        Item *it = malloc(sizeof(Item) * (size_t)n), *cp = malloc(sizeof(Item) * (size_t)n);
        Frac *tk = malloc(sizeof(Frac) * (size_t)n);
        long long wsum = 0;
        for (int i = 0; i < n; i++) {
            it[i] = (Item){i, 1 + (long long)(rnd() % 40), 1 + (long long)(rnd() % 100)};
            cp[i] = it[i];
            wsum += it[i].w;
        }
        long long cap = wsum / 3;
        Frac f = fractional(it, n, cap, tk);
        long long z = knap01(cp, n, cap);
        /* LP relaxation dominates the integer optimum */
        check(f.num >= z * f.den, "fractional bound dominates 0/1 optimum");
        /* fractions taken are within [0,1] and at most one is fractional */
        int partial = 0;
        Frac used = {0, 1};
        for (int i = 0; i < n; i++) {
            check(tk[i].num >= 0 && tk[i].num <= tk[i].den, "fraction in range");
            if (tk[i].num != 0 && tk[i].num != tk[i].den)
                partial++;
            used = add(used, mk(tk[i].num * cp[i].w, tk[i].den));
        }
        check(partial <= 1, "at most one fractional item");
        check(used.den == 1 && used.num == cap, "capacity exactly used");
        printf("n=%2d cap=%3lld fractional=", n, cap);
        show(f);
        printf(" zero-one=%lld\n", z);
        free(it);
        free(cp);
        free(tk);
    }
    return 0;
}
