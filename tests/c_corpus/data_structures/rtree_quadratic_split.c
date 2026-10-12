/*
 * title: R-tree with quadratic split
 * topic: data_structures
 * covers: r-tree, minimum bounding rectangles, choose-leaf by enlargement, quadratic split, condense-tree deletion, window queries
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M 6
#define MIN 2

typedef struct { int x1, y1, x2, y2; } Rect;
typedef struct RN {
    int height; /* 0 = leaf */
    int n;
    Rect r[M + 1];
    int id[M + 1];            /* leaf entries */
    struct RN *ch[M + 1];     /* internal entries */
} RN;

static unsigned long long rs = 0x8BADF00D5EEDULL * 3;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static long area(Rect a) { return (long)(a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1); }
static Rect uni(Rect a, Rect b) {
    Rect r = { a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1, a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2 };
    return r;
}
static int inter(Rect a, Rect b) { return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2; }
static int same(Rect a, Rect b) { return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2; }
static long enlarge(Rect a, Rect b) { return area(uni(a, b)) - area(a); }
static Rect mbr(const RN *n) { Rect r = n->r[0]; for (int i = 1; i < n->n; i++) r = uni(r, n->r[i]); return r; }

static RN *mk(int h) { RN *n = calloc(1, sizeof *n); n->height = h; return n; }
static void put(RN *n, Rect r, int id, RN *c) { n->r[n->n] = r; n->id[n->n] = id; n->ch[n->n] = c; n->n++; }

static int splits;
/* quadratic split of an overfull node n into n and a new node (returned) */
static RN *split(RN *n) {
    splits++;
    Rect r[M + 1]; int id[M + 1]; RN *ch[M + 1]; int cnt = n->n;
    memcpy(r, n->r, sizeof r); memcpy(id, n->id, sizeof id); memcpy(ch, n->ch, sizeof ch);
    int s1 = 0, s2 = 1; long worst = -1;
    for (int i = 0; i < cnt; i++) for (int j = i + 1; j < cnt; j++) {
        long d = area(uni(r[i], r[j])) - area(r[i]) - area(r[j]);
        if (d > worst) { worst = d; s1 = i; s2 = j; }
    }
    RN *m = mk(n->height);
    n->n = 0;
    put(n, r[s1], id[s1], ch[s1]); put(m, r[s2], id[s2], ch[s2]);
    Rect g1 = r[s1], g2 = r[s2];
    int used[M + 1] = {0}; used[s1] = used[s2] = 1; int left = cnt - 2;
    while (left > 0) {
        if (n->n + left == MIN) { for (int i = 0; i < cnt; i++) if (!used[i]) { put(n, r[i], id[i], ch[i]); g1 = uni(g1, r[i]); used[i] = 1; } break; }
        if (m->n + left == MIN) { for (int i = 0; i < cnt; i++) if (!used[i]) { put(m, r[i], id[i], ch[i]); g2 = uni(g2, r[i]); used[i] = 1; } break; }
        int pick = -1; long bestdiff = -1;
        for (int i = 0; i < cnt; i++) if (!used[i]) {
            long d1 = enlarge(g1, r[i]), d2 = enlarge(g2, r[i]);
            long diff = d1 > d2 ? d1 - d2 : d2 - d1;
            if (diff > bestdiff) { bestdiff = diff; pick = i; }
        }
        long d1 = enlarge(g1, r[pick]), d2 = enlarge(g2, r[pick]);
        int to1 = d1 < d2 || (d1 == d2 && (area(g1) < area(g2) || (area(g1) == area(g2) && n->n <= m->n)));
        if (to1) { put(n, r[pick], id[pick], ch[pick]); g1 = uni(g1, r[pick]); }
        else { put(m, r[pick], id[pick], ch[pick]); g2 = uni(g2, r[pick]); }
        used[pick] = 1; left--;
    }
    return m;
}
/* insert entry at the given level (0 = leaf); may return a new sibling from a split */
static RN *insert_rec(RN *n, Rect r, int id, RN *c, int level) {
    if (n->height == level) put(n, r, id, c);
    else {
        int best = 0; long be = enlarge(n->r[0], r), ba = area(n->r[0]);
        for (int i = 1; i < n->n; i++) {
            long e = enlarge(n->r[i], r), a = area(n->r[i]);
            if (e < be || (e == be && a < ba)) { best = i; be = e; ba = a; }
        }
        RN *sp = insert_rec(n->ch[best], r, id, c, level);
        n->r[best] = mbr(n->ch[best]);
        if (sp) put(n, mbr(sp), 0, sp);
    }
    return n->n > M ? split(n) : NULL;
}
static void insert(RN **root, Rect r, int id, RN *c, int level) {
    RN *sp = insert_rec(*root, r, id, c, level);
    if (sp) { RN *nr = mk((*root)->height + 1); put(nr, mbr(*root), 0, *root); put(nr, mbr(sp), 0, sp); *root = nr; }
}

typedef struct { Rect r; int id; RN *c; int level; } Orphan;
static Orphan orph[256]; static int norph;
static int remove_rec(RN *n, Rect r, int id) {
    if (n->height == 0) {
        for (int i = 0; i < n->n; i++) if (n->id[i] == id && same(n->r[i], r)) { n->r[i] = n->r[n->n - 1]; n->id[i] = n->id[n->n - 1]; n->n--; return 1; }
        return 0;
    }
    for (int i = 0; i < n->n; i++) {
        if (!inter(n->r[i], r)) continue;
        RN *c = n->ch[i];
        if (!remove_rec(c, r, id)) continue;
        if (c->n < MIN) { /* dissolve underfull child, queue its entries for reinsertion */
            for (int k = 0; k < c->n; k++) { orph[norph].r = c->r[k]; orph[norph].id = c->id[k]; orph[norph].c = c->ch[k]; orph[norph].level = c->height; norph++; }
            free(c);
            n->r[i] = n->r[n->n - 1]; n->ch[i] = n->ch[n->n - 1]; n->n--;
        } else n->r[i] = mbr(c);
        return 1;
    }
    return 0;
}
static int erase(RN **root, Rect r, int id) {
    norph = 0;
    if (!remove_rec(*root, r, id)) return 0;
    for (int i = 0; i < norph; i++) insert(root, orph[i].r, orph[i].id, orph[i].c, orph[i].level);
    while ((*root)->height > 0 && (*root)->n == 1) { RN *old = *root; *root = old->ch[0]; free(old); }
    return 1;
}
static int search(const RN *n, Rect q, int *out, int cnt, long *touched) {
    (*touched)++;
    for (int i = 0; i < n->n; i++) {
        if (!inter(n->r[i], q)) continue;
        if (n->height == 0) out[cnt++] = n->id[i]; else cnt = search(n->ch[i], q, out, cnt, touched);
    }
    return cnt;
}
static int verify(const RN *n, int is_root, int *leafdepth, int depth, int *nodes) {
    (*nodes)++;
    check(n->n <= M && (is_root || n->n >= MIN), "fill bounds");
    int total = 0;
    if (n->height == 0) {
        if (*leafdepth < 0) *leafdepth = depth; else check(*leafdepth == depth, "all leaves at the same depth");
        return n->n;
    }
    for (int i = 0; i < n->n; i++) {
        check(n->ch[i]->height == n->height - 1, "child height");
        check(same(n->r[i], mbr(n->ch[i])), "parent rectangle is the tight MBR");
        total += verify(n->ch[i], 0, leafdepth, depth + 1, nodes);
    }
    return total;
}
static void destroy(RN *n) { if (n->height) for (int i = 0; i < n->n; i++) destroy(n->ch[i]); free(n); }

#define NR 1500
static Rect R[NR];
static char alive[NR];

int main(void) {
    RN *root = mk(0);
    for (int i = 0; i < NR; i++) {
        int x, y, w = 1 + (int)(rnd() % 30), h = 1 + (int)(rnd() % 30);
        if (i % 2) { x = (int)(rnd() % 970); y = (int)(rnd() % 970); }
        else { x = 300 + (int)(rnd() % 200); y = 400 + (int)(rnd() % 200); }
        R[i].x1 = x; R[i].y1 = y; R[i].x2 = x + w; R[i].y2 = y + h;
        insert(&root, R[i], i, NULL, 0); alive[i] = 1;
        if (i % 300 == 299) {
            int ld = -1, nodes = 0; int tot = verify(root, 1, &ld, 0, &nodes);
            check(tot == i + 1, "entry count");
            printf("after %4d inserts: height %d nodes %d splits %d\n", i + 1, root->height, nodes, splits);
        }
    }
    long touched = 0, hits = 0;
    for (int q = 0; q < 200; q++) {
        Rect w; w.x1 = (int)(rnd() % 950); w.y1 = (int)(rnd() % 950); w.x2 = w.x1 + 10 + (int)(rnd() % 80); w.y2 = w.y1 + 10 + (int)(rnd() % 80);
        static int out[NR]; long t = 0;
        int n = search(root, w, out, 0, &t);
        int want = 0;
        for (int i = 0; i < NR; i++) if (alive[i] && inter(R[i], w)) want++;
        check(n == want, "window count");
        touched += t; hits += n;
    }
    printf("200 window queries: %ld hits, avg nodes touched %ld\n", hits, touched / 200);
    int del = 0;
    for (int i = 0; i < NR; i++) if (i % 5 != 0 && i % 7 != 0) { check(erase(&root, R[i], i), "erase existing"); alive[i] = 0; del++; }
    check(!erase(&root, R[1], 1), "erasing twice fails");
    int ld = -1, nodes = 0; int tot = verify(root, 1, &ld, 0, &nodes);
    check(tot == NR - del, "entries after deletion");
    long hits2 = 0;
    for (int q = 0; q < 200; q++) {
        Rect w; w.x1 = (int)(rnd() % 950); w.y1 = (int)(rnd() % 950); w.x2 = w.x1 + 20 + (int)(rnd() % 100); w.y2 = w.y1 + 20 + (int)(rnd() % 100);
        static int out[NR]; long t = 0;
        int n = search(root, w, out, 0, &t);
        int want = 0;
        for (int i = 0; i < NR; i++) if (alive[i] && inter(R[i], w)) want++;
        check(n == want, "window count after deletes");
        hits2 += n;
    }
    printf("deleted %d entries: %d remain, height %d nodes %d, 200 window queries: %ld hits\n", del, tot, root->height, nodes, hits2);
    destroy(root);
    return 0;
}
