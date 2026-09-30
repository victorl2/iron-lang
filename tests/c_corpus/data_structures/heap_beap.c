/*
 * title: Beap (bi-parental heap) with saddleback search
 * topic: data_structures
 * covers: beap, bi-parental heap, triangular row layout, Young-tableau saddleback search, delete by value
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0xBEA9ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Row r (0-based) holds r+1 items at indices r(r+1)/2 .. r(r+1)/2 + r.
 * Item (r,c) has parents (r-1,c-1),(r-1,c) and children (r+1,c),(r+1,c+1). */
enum { CAP = 2000 };
typedef struct {
    int a[CAP];
    int n;
    long search_steps;
} Beap;

static int idx(int r, int c) { return r * (r + 1) / 2 + c; }

static void rc_of(int i, int *r, int *c) {
    int row = 0;
    while (idx(row + 1, 0) <= i)
        row++;
    *r = row;
    *c = i - idx(row, 0);
}

static void swp(Beap *b, int i, int j) {
    int t = b->a[i];
    b->a[i] = b->a[j];
    b->a[j] = t;
}

static int sift_up(Beap *b, int i) {
    for (;;) {
        int r, c;
        rc_of(i, &r, &c);
        if (r == 0)
            return i;
        int q = -1;
        if (c > 0)
            q = idx(r - 1, c - 1);
        if (c < r) {
            int p2 = idx(r - 1, c);
            if (q < 0 || b->a[p2] > b->a[q])
                q = p2;
        }
        if (b->a[i] >= b->a[q])
            return i;
        swp(b, i, q);
        i = q;
    }
}

static void sift_down(Beap *b, int i) {
    for (;;) {
        int r, c;
        rc_of(i, &r, &c);
        int c1 = idx(r + 1, c), c2 = idx(r + 1, c + 1), m = -1;
        if (c1 < b->n)
            m = c1;
        if (c2 < b->n && (m < 0 || b->a[c2] < b->a[m]))
            m = c2;
        if (m < 0 || b->a[i] <= b->a[m])
            return;
        swp(b, i, m);
        i = m;
    }
}

static void push(Beap *b, int x) {
    check(b->n < CAP, "capacity");
    b->a[b->n] = x;
    sift_up(b, b->n);
    b->n++;
}

static void remove_at(Beap *b, int i) {
    b->n--;
    if (i == b->n)
        return;
    b->a[i] = b->a[b->n];
    int j = sift_up(b, i);
    sift_down(b, j);
}

/* Saddleback search in the (u,v) = (c, r-c) staircase: values grow with u and with v. */
static int exists(const Beap *b, int u, int v) { return idx(u + v, u) < b->n; }

static int find(Beap *b, int x) {
    int u = 0;
    while (exists(b, u + 1, 0))
        u++;
    int v = 0;
    while (u >= 0) {
        b->search_steps++;
        if (!exists(b, u, v) || b->a[idx(u + v, u)] > x)
            u--;
        else if (b->a[idx(u + v, u)] < x)
            v++;
        else
            return idx(u + v, u);
    }
    return -1;
}

static void invariant(const Beap *b) {
    for (int i = 1; i < b->n; i++) {
        int r, c;
        rc_of(i, &r, &c);
        if (c > 0)
            check(b->a[idx(r - 1, c - 1)] <= b->a[i], "left parent");
        if (c < r)
            check(b->a[idx(r - 1, c)] <= b->a[i], "right parent");
    }
}

int main(void) {
    static Beap b;
    int model[CAP], mn = 0;
    int pushes = 0, popmins = 0, delvals = 0, hits = 0, misses = 0;
    long popsum = 0;
    for (int op = 0; op < 9000; op++) {
        int r = (int)(rng() % 100);
        if ((r < 43 || mn == 0) && mn < CAP - 1) {
            int v = (int)(rng() % 1500);
            model[mn++] = v;
            push(&b, v);
            pushes++;
        } else if (r < 66) {
            int bi = 0;
            for (int i = 1; i < mn; i++)
                if (model[i] < model[bi])
                    bi = i;
            check(b.a[0] == model[bi], "root is the minimum");
            popsum += b.a[0];
            model[bi] = model[--mn];
            remove_at(&b, 0);
            popmins++;
        } else if (r < 82) {
            int v = (int)(rng() % 1500), present = 0, mi = -1;
            for (int i = 0; i < mn; i++)
                if (model[i] == v) {
                    present = 1;
                    mi = i;
                }
            int pos = find(&b, v);
            check((pos >= 0) == present, "search agrees with model");
            if (pos >= 0) {
                check(b.a[pos] == v, "found the value");
                hits++;
                if (r < 74) {
                    model[mi] = model[--mn];
                    remove_at(&b, pos);
                    delvals++;
                }
            } else
                misses++;
        }
        check(b.n == mn, "size");
        invariant(&b);
    }
    printf("pushes=%d pop_min=%d delete_value=%d hits=%d misses=%d\n", pushes, popmins, delvals, hits, misses);
    printf("size=%d popsum=%ld search_steps=%ld\n", b.n, popsum, b.search_steps);
    int rows = 0, c;
    rc_of(b.n - 1, &rows, &c);
    printf("rows=%d last_row_items=%d\n", rows + 1, c + 1);
    return 0;
}
