/*
 * title: Point-region octree
 * topic: data_structures
 * covers: octree, spatial subdivision, morton order traversal, box queries, nearest neighbour, deletion with node merging
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NP 3000
#define BOX 1024
#define CAP 6
#define MAXDEPTH 10

typedef struct Oct {
    int x0, y0, z0, size;      /* cube [x0, x0+size) ... */
    int n;                      /* points stored when leaf */
    int pts[CAP];
    struct Oct *kid[8];         /* NULL when leaf */
    int count;                  /* points in subtree */
} Oct;

static int PX[NP], PY[NP], PZ[NP];
static char alive[NP];

static unsigned long long rs = 0x0C7EE0C7EEULL * 65537;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static Oct *mk(int x, int y, int z, int size) {
    Oct *o = calloc(1, sizeof *o);
    o->x0 = x; o->y0 = y; o->z0 = z; o->size = size;
    return o;
}
static int octant(const Oct *o, int p) {
    int h = o->size / 2;
    return (PZ[p] >= o->z0 + h ? 4 : 0) | (PY[p] >= o->y0 + h ? 2 : 0) | (PX[p] >= o->x0 + h ? 1 : 0);
}
static int depth_of(const Oct *o) { int d = 0; for (int s = BOX; s > o->size; s /= 2) d++; return d; }
static void insert(Oct *o, int p) {
    o->count++;
    if (!o->kid[0]) {
        if (o->n < CAP) { o->pts[o->n++] = p; return; }
        check(depth_of(o) < MAXDEPTH, "bucket overflow at maximum depth");
        int h = o->size / 2;
        for (int c = 0; c < 8; c++) o->kid[c] = mk(o->x0 + (c & 1 ? h : 0), o->y0 + (c & 2 ? h : 0), o->z0 + (c & 4 ? h : 0), h);
        int old[CAP], no = o->n; memcpy(old, o->pts, sizeof old); o->n = 0;
        for (int i = 0; i < no; i++) insert(o->kid[octant(o, old[i])], old[i]);
    }
    insert(o->kid[octant(o, p)], p);
}
static int erase(Oct *o, int p) {
    if (!o->kid[0]) {
        for (int i = 0; i < o->n; i++) if (o->pts[i] == p) { o->pts[i] = o->pts[--o->n]; o->count--; return 1; }
        return 0;
    }
    int c = octant(o, p);
    if (!erase(o->kid[c], p)) return 0;
    o->count--;
    if (o->count <= CAP) { /* merge children back into this node */
        int m = 0;
        for (int k = 0; k < 8; k++) { for (int i = 0; i < o->kid[k]->n; i++) o->pts[m++] = o->kid[k]->pts[i]; free(o->kid[k]); o->kid[k] = NULL; }
        o->n = m;
    }
    return 1;
}
static void destroy(Oct *o) { if (o->kid[0]) for (int i = 0; i < 8; i++) destroy(o->kid[i]); free(o); }

static int in_box(int p, const int *lo, const int *hi) {
    return PX[p] >= lo[0] && PX[p] <= hi[0] && PY[p] >= lo[1] && PY[p] <= hi[1] && PZ[p] >= lo[2] && PZ[p] <= hi[2];
}
static long nodes_touched;
static int box_query(const Oct *o, const int *lo, const int *hi, int *out, int cnt) {
    nodes_touched++;
    if (o->x0 > hi[0] || o->x0 + o->size - 1 < lo[0] || o->y0 > hi[1] || o->y0 + o->size - 1 < lo[1] || o->z0 > hi[2] || o->z0 + o->size - 1 < lo[2]) return cnt;
    if (!o->kid[0]) { for (int i = 0; i < o->n; i++) if (in_box(o->pts[i], lo, hi)) out[cnt++] = o->pts[i]; return cnt; }
    for (int c = 0; c < 8; c++) cnt = box_query(o->kid[c], lo, hi, out, cnt);
    return cnt;
}
static long box_dist2(const Oct *o, int x, int y, int z) {
    long dx = x < o->x0 ? o->x0 - x : (x > o->x0 + o->size - 1 ? x - (o->x0 + o->size - 1) : 0);
    long dy = y < o->y0 ? o->y0 - y : (y > o->y0 + o->size - 1 ? y - (o->y0 + o->size - 1) : 0);
    long dz = z < o->z0 ? o->z0 - z : (z > o->z0 + o->size - 1 ? z - (o->z0 + o->size - 1) : 0);
    return dx * dx + dy * dy + dz * dz;
}
static long pd2(int p, int x, int y, int z) { long dx = PX[p] - x, dy = PY[p] - y, dz = PZ[p] - z; return dx * dx + dy * dy + dz * dz; }
static void nearest(const Oct *o, int x, int y, int z, int *best, long *bd) {
    if (o->count == 0 || box_dist2(o, x, y, z) > *bd) return;
    if (!o->kid[0]) {
        for (int i = 0; i < o->n; i++) { long d = pd2(o->pts[i], x, y, z); if (d < *bd || (d == *bd && o->pts[i] < *best)) { *bd = d; *best = o->pts[i]; } }
        return;
    }
    for (int c = 0; c < 8; c++) nearest(o->kid[c], x, y, z, best, bd);
}
static unsigned long long morton(int x, int y, int z) {
    unsigned long long m = 0;
    for (int b = 9; b >= 0; b--) m = (m << 3) | ((unsigned long long)((z >> b) & 1) << 2) | ((unsigned long long)((y >> b) & 1) << 1) | (unsigned long long)((x >> b) & 1);
    return m;
}
static int walk(const Oct *o, int *out, int cnt) {
    if (!o->kid[0]) {
        /* sort leaf points by morton to make traversal order fully canonical */
        int t[CAP], n = o->n; memcpy(t, o->pts, sizeof(int) * (size_t)n);
        for (int i = 1; i < n; i++) { int v = t[i], j = i - 1; while (j >= 0 && (morton(PX[t[j]], PY[t[j]], PZ[t[j]]) > morton(PX[v], PY[v], PZ[v]) || (morton(PX[t[j]], PY[t[j]], PZ[t[j]]) == morton(PX[v], PY[v], PZ[v]) && t[j] > v))) { t[j + 1] = t[j]; j--; } t[j + 1] = v; }
        for (int i = 0; i < n; i++) out[cnt++] = t[i];
        return cnt;
    }
    for (int c = 0; c < 8; c++) cnt = walk(o->kid[c], out, cnt);
    return cnt;
}
static int nodes(const Oct *o) { int c = 1; if (o->kid[0]) for (int i = 0; i < 8; i++) c += nodes(o->kid[i]); return c; }
static int leaves(const Oct *o) { if (!o->kid[0]) return 1; int c = 0; for (int i = 0; i < 8; i++) c += leaves(o->kid[i]); return c; }
static int maxdepth(const Oct *o) { if (!o->kid[0]) return 0; int m = 0; for (int i = 0; i < 8; i++) { int d = maxdepth(o->kid[i]); if (d > m) m = d; } return m + 1; }
static void check_counts(const Oct *o) {
    if (!o->kid[0]) { check(o->count == o->n, "leaf count"); return; }
    int s = 0; for (int i = 0; i < 8; i++) { s += o->kid[i]->count; check_counts(o->kid[i]); }
    check(s == o->count && o->count > CAP, "internal count and merge invariant");
}
static int cmp_morton(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    unsigned long long mx = morton(PX[x], PY[x], PZ[x]), my = morton(PX[y], PY[y], PZ[y]);
    if (mx != my) return mx < my ? -1 : 1;
    return x < y ? -1 : x > y;
}

int main(void) {
    Oct *root = mk(0, 0, 0, BOX);
    static unsigned char cell[BOX / 4][BOX / 4][BOX / 4 / 8]; /* one point per 4x4x4 cell so subdivision terminates */
    int n = 0;
    while (n < NP) {
        int x, y, z;
        if (n % 3) { int c = (int)(rnd() % 5); x = (c * 200 + (int)(rnd() % 60)) % BOX; y = (c * 130 + (int)(rnd() % 60)) % BOX; z = (c * 90 + 100 + (int)(rnd() % 60)) % BOX; }
        else { x = (int)(rnd() % BOX); y = (int)(rnd() % BOX); z = (int)(rnd() % BOX); }
        int cx = x / 4, cy = y / 4, cz = z / 4;
        if (cell[cx][cy][cz >> 3] & (1u << (cz & 7))) continue;
        cell[cx][cy][cz >> 3] |= (unsigned char)(1u << (cz & 7));
        PX[n] = x; PY[n] = y; PZ[n] = z; alive[n] = 1;
        insert(root, n); n++;
    }
    check_counts(root);
    printf("points %d nodes %d leaves %d max depth %d\n", n, nodes(root), leaves(root), maxdepth(root));
    /* traversal order equals morton order */
    static int ord[NP], ref[NP];
    int no = walk(root, ord, 0);
    check(no == NP, "walk count");
    for (int i = 0; i < NP; i++) ref[i] = i;
    qsort(ref, NP, sizeof(int), cmp_morton);
    for (int i = 0; i < NP; i++) check(ord[i] == ref[i], "DFS order is Morton order");
    printf("depth-first leaf order equals Morton order; first ids %d %d %d\n", ord[0], ord[1], ord[2]);
    long touched = 0, hits = 0;
    for (int q = 0; q < 200; q++) {
        int lo[3], hi[3];
        for (int d = 0; d < 3; d++) { lo[d] = (int)(rnd() % BOX); hi[d] = lo[d] + 20 + (int)(rnd() % 200); if (hi[d] >= BOX) hi[d] = BOX - 1; }
        static int got[NP]; nodes_touched = 0;
        int ng = box_query(root, lo, hi, got, 0);
        touched += nodes_touched;
        int want = 0;
        for (int i = 0; i < NP; i++) if (alive[i] && in_box(i, lo, hi)) want++;
        check(ng == want, "box query count");
        hits += ng;
    }
    printf("200 box queries: %ld hits, avg nodes touched %ld\n", hits, touched / 200);
    long sd = 0;
    for (int q = 0; q < 200; q++) {
        int x = (int)(rnd() % BOX), y = (int)(rnd() % BOX), z = (int)(rnd() % BOX);
        int best = -1; long bd = 1L << 60;
        nearest(root, x, y, z, &best, &bd);
        int wb = -1; long wd = 1L << 60;
        for (int i = 0; i < NP; i++) if (alive[i]) { long d = pd2(i, x, y, z); if (d < wd) { wd = d; wb = i; } }
        check(best == wb && bd == wd, "nearest");
        sd += bd;
    }
    printf("200 nearest queries: sum of squared distances %ld\n", sd);
    /* delete two thirds, verify merge invariant and queries again */
    int removed = 0;
    for (int i = 0; i < NP; i++) if (i % 3 != 0) { check(erase(root, i), "erase"); alive[i] = 0; removed++; }
    check(!erase(root, 1), "double erase fails");
    check_counts(root);
    int lo[3] = { 0, 0, 0 }, hi[3] = { BOX - 1, BOX - 1, BOX - 1 };
    static int got[NP];
    int all = box_query(root, lo, hi, got, 0);
    check(all == NP - removed && root->count == all, "remaining after deletes");
    printf("after deleting %d points: %d remain, nodes %d leaves %d max depth %d\n", removed, all, nodes(root), leaves(root), maxdepth(root));
    destroy(root);
    return 0;
}
