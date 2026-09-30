/*
 * title: Simplified cover tree
 * topic: data_structures
 * covers: cover tree, covering and separation invariants, nearest neighbour, range query, L1 metric, level-based radii
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP 1200
#define ROOTLEVEL 11 /* 2^11 = 2048 exceeds the maximum L1 distance in a 1024x1024 grid */

typedef struct CT {
    int pt, level;
    int nkids;
    struct CT **kids;
} CT;

static int X[NP], Y[NP];
static long dcalls;

static unsigned long long rs = 0x1D5A3F91ULL * 11939;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int dist(int a, int b) { dcalls++; return abs(X[a] - X[b]) + abs(Y[a] - Y[b]); }
static int dist_q(int qx, int qy, int b) { dcalls++; return abs(qx - X[b]) + abs(qy - Y[b]); }
static long radius(int level) { return level >= 0 ? 1L << level : 0; } /* level -1 has radius 0: only the point itself */

static CT *mknode(int pt, int level) {
    CT *n = calloc(1, sizeof *n);
    n->pt = pt; n->level = level;
    return n;
}
static void add_kid(CT *n, CT *k) {
    n->kids = realloc(n->kids, sizeof(CT *) * (size_t)(n->nkids + 1));
    n->kids[n->nkids++] = k;
}
/* precondition: dist(p, q->pt) <= radius(q->level) */
static void insert(CT *q, int p) {
    for (int i = 0; i < q->nkids; i++) {
        CT *c = q->kids[i];
        if (dist(p, c->pt) <= radius(c->level)) { insert(c, p); return; }
    }
    add_kid(q, mknode(p, q->level - 1));
}
static void destroy(CT *n) { for (int i = 0; i < n->nkids; i++) destroy(n->kids[i]); free(n->kids); free(n); }

/* every descendant lies within 2^(level+1) of a node at `level` (geometric series bound) */
static long desc_bound(int level) { return radius(level + 1); }

static void nn(const CT *n, int qx, int qy, int *best, int *bestd) {
    int d = dist_q(qx, qy, n->pt);
    if (d < *bestd || (d == *bestd && n->pt < *best)) { *bestd = d; *best = n->pt; }
    /* visit children nearest first */
    int order[64]; int dd[64]; int k = n->nkids;
    for (int i = 0; i < k; i++) { order[i] = i; dd[i] = dist_q(qx, qy, n->kids[i]->pt); }
    for (int i = 1; i < k; i++) { int o = order[i], v = dd[i], j = i - 1; while (j >= 0 && dd[j] > v) { dd[j + 1] = dd[j]; order[j + 1] = order[j]; j--; } dd[j + 1] = v; order[j + 1] = o; }
    for (int i = 0; i < k; i++) {
        const CT *c = n->kids[order[i]];
        if (dd[i] - desc_bound(c->level) <= *bestd) nn(c, qx, qy, best, bestd);
    }
}
static int nodes_total, max_depth, level_hist[16];
static void verify(const CT *n, int depth) {
    nodes_total++;
    if (depth > max_depth) max_depth = depth;
    level_hist[n->level + 1]++;
    for (int i = 0; i < n->nkids; i++) {
        const CT *c = n->kids[i];
        check(c->level == n->level - 1, "child one level below");
        check(dist(n->pt, c->pt) <= radius(n->level), "covering: child within parent radius");
        for (int j = i + 1; j < n->nkids; j++)
            check(dist(c->pt, n->kids[j]->pt) > radius(c->level), "separation among siblings");
        verify(c, depth + 1);
    }
}
/* every descendant within 2^(level+1) of ancestor */
static void check_desc(const CT *anc, const CT *n) {
    for (int i = 0; i < n->nkids; i++) {
        check(dist(anc->pt, n->kids[i]->pt) <= desc_bound(anc->level), "descendant bound");
        check_desc(anc, n->kids[i]);
    }
}

int main(void) {
    int n = 0;
    static unsigned char taken[1024][1024];
    /* points: mixture of uniform and clustered, distinct */
    while (n < NP) {
        int x, y;
        if (n % 2) { x = (int)(rnd() % 1024); y = (int)(rnd() % 1024); }
        else { int cx = 200 + 300 * (int)(rnd() % 3), cy = 300 + 250 * (int)(rnd() % 3); x = cx + (int)(rnd() % 90); y = cy + (int)(rnd() % 90); }
        if (x > 1023 || y > 1023 || taken[y][x]) continue;
        taken[y][x] = 1; X[n] = x; Y[n] = y; n++;
    }
    CT *root = mknode(0, ROOTLEVEL);
    for (int i = 1; i < NP; i++) insert(root, i);
    dcalls = 0;
    verify(root, 0);
    check(nodes_total == NP, "every point is a node exactly once");
    for (int i = 0; i < root->nkids; i++) check_desc(root, root->kids[i]);
    printf("points %d, max depth %d, root children %d\n", NP, max_depth, root->nkids);
    printf("nodes per level:");
    for (int l = 15; l >= 0; l--) if (level_hist[l]) printf(" L%d:%d", l - 1, level_hist[l]);
    printf("\n");
    long tc = 0, sumd = 0;
    for (int q = 0; q < 200; q++) {
        int qx = (int)(rnd() % 1024), qy = (int)(rnd() % 1024);
        int best = -1, bd = 1 << 30;
        dcalls = 0;
        nn(root, qx, qy, &best, &bd);
        tc += dcalls;
        int wb = -1, wd = 1 << 30;
        for (int i = 0; i < NP; i++) { int d = abs(qx - X[i]) + abs(qy - Y[i]); if (d < wd) { wd = d; wb = i; } }
        check(bd == wd && best == wb, "nearest neighbour matches brute force");
        sumd += bd;
    }
    printf("200 nearest queries: avg distance calls %ld (brute %d), sum of distances %ld\n", tc / 200, NP, sumd);
    for (int r = 20; r <= 160; r *= 2) {
        long tot = 0;
        for (int q = 0; q < 60; q++) {
            int qx = (int)(rnd() % 1024), qy = (int)(rnd() % 1024);
            /* count via a fresh traversal that counts each node once */
            int c = 0;
            /* explicit stack traversal with pruning */
            const CT *st[NP]; int sp = 0; st[sp++] = root;
            while (sp) {
                const CT *nd = st[--sp];
                if (dist_q(qx, qy, nd->pt) <= r) c++;
                for (int i = 0; i < nd->nkids; i++) {
                    const CT *k = nd->kids[i];
                    if (dist_q(qx, qy, k->pt) - desc_bound(k->level) <= r) st[sp++] = k;
                }
            }
            int want = 0;
            for (int i = 0; i < NP; i++) if (abs(qx - X[i]) + abs(qy - Y[i]) <= r) want++;
            check(c == want, "range count vs brute force");
            tot += c;
        }
        printf("L1 radius %3d: total hits over 60 queries %ld\n", r, tot);
    }
    destroy(root);
    return 0;
}
