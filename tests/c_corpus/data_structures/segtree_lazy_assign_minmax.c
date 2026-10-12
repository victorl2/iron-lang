/*
 * title: Lazy segment tree with composed assign and add tags
 * topic: data_structures
 * covers: lazy tag composition, assign overrides add, range min, range max, range sum, struct nodes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    long sum, mn, mx;
    int len;
    int has_set; /* pending assignment */
    long set_v;
    long add_v; /* pending add, applied after any pending assignment */
} Node;

#define MAXN 300
static Node t[4 * MAXN];

static void pull(int o) {
    t[o].sum = t[2 * o].sum + t[2 * o + 1].sum;
    t[o].mn = t[2 * o].mn < t[2 * o + 1].mn ? t[2 * o].mn : t[2 * o + 1].mn;
    t[o].mx = t[2 * o].mx > t[2 * o + 1].mx ? t[2 * o].mx : t[2 * o + 1].mx;
}

static void tag_set(int o, long v) {
    t[o].sum = v * t[o].len;
    t[o].mn = t[o].mx = v;
    t[o].has_set = 1;
    t[o].set_v = v;
    t[o].add_v = 0;
}

static void tag_add(int o, long v) {
    t[o].sum += v * t[o].len;
    t[o].mn += v;
    t[o].mx += v;
    if (t[o].has_set)
        t[o].set_v += v;
    else
        t[o].add_v += v;
}

static void push(int o) {
    if (t[o].has_set) {
        tag_set(2 * o, t[o].set_v);
        tag_set(2 * o + 1, t[o].set_v);
        t[o].has_set = 0;
    }
    if (t[o].add_v) {
        tag_add(2 * o, t[o].add_v);
        tag_add(2 * o + 1, t[o].add_v);
        t[o].add_v = 0;
    }
}

static void build(int o, int l, int r, const long *a) {
    memset(&t[o], 0, sizeof t[o]);
    t[o].len = r - l + 1;
    if (l == r) {
        t[o].sum = t[o].mn = t[o].mx = a[l];
        return;
    }
    int m = (l + r) / 2;
    build(2 * o, l, m, a);
    build(2 * o + 1, m + 1, r, a);
    pull(o);
}

/* mode 0 = assign, 1 = add */
static void update(int o, int l, int r, int ql, int qr, int mode, long v) {
    if (qr < l || r < ql)
        return;
    if (ql <= l && r <= qr) {
        if (mode == 0)
            tag_set(o, v);
        else
            tag_add(o, v);
        return;
    }
    push(o);
    int m = (l + r) / 2;
    update(2 * o, l, m, ql, qr, mode, v);
    update(2 * o + 1, m + 1, r, ql, qr, mode, v);
    pull(o);
}

typedef struct {
    long sum, mn, mx;
} Res;

static Res query(int o, int l, int r, int ql, int qr) {
    if (ql <= l && r <= qr) {
        Res x = {t[o].sum, t[o].mn, t[o].mx};
        return x;
    }
    push(o);
    int m = (l + r) / 2;
    if (qr <= m)
        return query(2 * o, l, m, ql, qr);
    if (ql > m)
        return query(2 * o + 1, m + 1, r, ql, qr);
    Res a = query(2 * o, l, m, ql, qr);
    Res b = query(2 * o + 1, m + 1, r, ql, qr);
    Res c = {a.sum + b.sum, a.mn < b.mn ? a.mn : b.mn, a.mx > b.mx ? a.mx : b.mx};
    return c;
}

int main(void) {
    int n = 300;
    long a[MAXN], ref[MAXN];
    for (int i = 0; i < n; i++)
        a[i] = ref[i] = (long)rnd(100);
    build(1, 0, n - 1, a);
    long checksum = 0;
    int nset = 0, nadd = 0, nq = 0;
    for (int step = 0; step < 6000; step++) {
        int l = (int)rnd((unsigned)n);
        int r = (int)rnd((unsigned)n);
        if (l > r) {
            int x = l;
            l = r;
            r = x;
        }
        unsigned op = rnd(3);
        if (op == 0) {
            long v = (long)rnd(1000);
            update(1, 0, n - 1, l, r, 0, v);
            for (int i = l; i <= r; i++)
                ref[i] = v;
            nset++;
        } else if (op == 1) {
            long v = (long)rnd(21) - 10;
            update(1, 0, n - 1, l, r, 1, v);
            for (int i = l; i <= r; i++)
                ref[i] += v;
            nadd++;
        } else {
            Res g = query(1, 0, n - 1, l, r);
            long s = 0, mn = ref[l], mx = ref[l];
            for (int i = l; i <= r; i++) {
                s += ref[i];
                if (ref[i] < mn)
                    mn = ref[i];
                if (ref[i] > mx)
                    mx = ref[i];
            }
            check(g.sum == s && g.mn == mn && g.mx == mx, "query triple");
            checksum = (checksum * 1000003 + g.sum + 7 * g.mn + 13 * g.mx) % 1000000007L;
            nq++;
        }
    }
    printf("assigns=%d adds=%d queries=%d\n", nset, nadd, nq);
    printf("checksum=%ld\n", checksum);
    Res all = query(1, 0, n - 1, 0, n - 1);
    printf("final sum=%ld min=%ld max=%ld\n", all.sum, all.mn, all.mx);
    /* explicit composition scenario: add, assign, add */
    long z[4] = {1, 2, 3, 4};
    build(1, 0, 3, z);
    update(1, 0, 3, 0, 3, 1, 10);
    update(1, 0, 3, 0, 3, 0, 5);
    update(1, 0, 3, 0, 3, 1, 2);
    Res c = query(1, 0, 3, 0, 3);
    check(c.sum == 28 && c.mn == 7 && c.mx == 7, "add, assign, add");
    printf("composed: sum=%ld min=%ld max=%ld\n", c.sum, c.mn, c.mx);
    return 0;
}
