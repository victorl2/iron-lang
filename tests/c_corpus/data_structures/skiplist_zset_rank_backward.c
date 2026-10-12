/*
 * title: Sorted-set skip list with backward links and rank queries
 * topic: data_structures
 * covers: skip list sorted set, score and member ordering, backward pointer, spans, rank of member, range by rank, score updates, hash index
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLVL 16
#define M 300   /* member ids 0..M-1 */

static unsigned long long rs = 0x25E7ULL * 0x9E3779B1ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct Node Node;
struct Node {
    int member, score, lvl;
    Node *back;
    struct { Node *fwd; int span; } lv[MAXLVL];
};
typedef struct { Node *head, *tail; int len, level; } ZSet;

static Node *by_member[M];   /* index: member -> node (like the dict beside a zset) */

static int less(int s1, int m1, int s2, int m2) { return s1 < s2 || (s1 == s2 && m1 < m2); }
static Node *mk(int lvl, int score, int member) { Node *n = calloc(1, sizeof *n); n->lvl = lvl; n->score = score; n->member = member; return n; }
static void zinit(ZSet *z) { z->head = mk(MAXLVL, 0, -1); z->tail = NULL; z->len = 0; z->level = 1; }
static int rlevel(void) { int l = 1; while (l < MAXLVL && (rnd() & 3u) == 0) l++; return l; }

static void zadd(ZSet *z, int score, int member) {
    Node *upd[MAXLVL]; int rank[MAXLVL];
    Node *x = z->head;
    for (int i = z->level - 1; i >= 0; i--) {
        rank[i] = i == z->level - 1 ? 0 : rank[i + 1];
        while (x->lv[i].fwd && less(x->lv[i].fwd->score, x->lv[i].fwd->member, score, member)) { rank[i] += x->lv[i].span; x = x->lv[i].fwd; }
        upd[i] = x;
    }
    int lvl = rlevel();
    if (lvl > z->level) {
        for (int i = z->level; i < lvl; i++) { rank[i] = 0; upd[i] = z->head; upd[i]->lv[i].span = z->len; }
        z->level = lvl;
    }
    x = mk(lvl, score, member);
    for (int i = 0; i < lvl; i++) {
        x->lv[i].fwd = upd[i]->lv[i].fwd; upd[i]->lv[i].fwd = x;
        x->lv[i].span = upd[i]->lv[i].span - (rank[0] - rank[i]);
        upd[i]->lv[i].span = (rank[0] - rank[i]) + 1;
    }
    for (int i = lvl; i < z->level; i++) upd[i]->lv[i].span++;
    x->back = upd[0] == z->head ? NULL : upd[0];
    if (x->lv[0].fwd) x->lv[0].fwd->back = x; else z->tail = x;
    z->len++;
    by_member[member] = x;
}
static void zdel(ZSet *z, Node *t) {
    Node *upd[MAXLVL]; Node *x = z->head;
    for (int i = z->level - 1; i >= 0; i--) {
        while (x->lv[i].fwd && less(x->lv[i].fwd->score, x->lv[i].fwd->member, t->score, t->member)) x = x->lv[i].fwd;
        upd[i] = x;
    }
    for (int i = 0; i < z->level; i++) {
        if (upd[i]->lv[i].fwd == t) { upd[i]->lv[i].span += t->lv[i].span - 1; upd[i]->lv[i].fwd = t->lv[i].fwd; }
        else upd[i]->lv[i].span--;
    }
    if (t->lv[0].fwd) t->lv[0].fwd->back = t->back; else z->tail = t->back;
    while (z->level > 1 && !z->head->lv[z->level - 1].fwd) z->level--;
    z->len--;
    by_member[t->member] = NULL;
    free(t);
}
static int zrank(const ZSet *z, const Node *t) {  /* 1-based rank */
    int r = 0; const Node *x = z->head;
    for (int i = z->level - 1; i >= 0; i--) {
        while (x->lv[i].fwd && less(x->lv[i].fwd->score, x->lv[i].fwd->member, t->score, t->member)) { r += x->lv[i].span; x = x->lv[i].fwd; }
    }
    return r + x->lv[0].span;   /* one step forward reaches t itself */
}
static Node *zbyrank(const ZSet *z, int rank) {
    int at = 0; Node *x = z->head;
    for (int i = z->level - 1; i >= 0; i--) while (x->lv[i].fwd && at + x->lv[i].span <= rank) { at += x->lv[i].span; x = x->lv[i].fwd; }
    return at == rank ? x : NULL;
}

/* oracle */
static int has[M], sc[M];
static int order_ref[M];
static int cmp_ref(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (sc[x] != sc[y]) return sc[x] < sc[y] ? -1 : 1;
    return (x > y) - (x < y);
}
static int build_ref(void) { int n = 0; for (int i = 0; i < M; i++) if (has[i]) order_ref[n++] = i; qsort(order_ref, (size_t)n, sizeof(int), cmp_ref); return n; }

int main(void) {
    ZSet z; zinit(&z);
    int adds = 0, updates = 0, removes = 0;
    for (int step = 0; step < 5000; step++) {
        int m = (int)(rnd() % M); unsigned op = rnd() % 10;
        int score = (int)(rnd() % 50);
        if (op < 5) {
            if (has[m]) { zdel(&z, by_member[m]); updates++; } else adds++;
            zadd(&z, score, m); has[m] = 1; sc[m] = score;
        } else if (op < 7) {
            if (has[m]) { zdel(&z, by_member[m]); has[m] = 0; removes++; }
        } else if (has[m] || op == 9) {
            if (!has[m]) continue;
            int n = build_ref(), r = 0;
            for (int i = 0; i < n; i++) if (order_ref[i] == m) r = i + 1;
            check(zrank(&z, by_member[m]) == r, "rank of member");
            Node *b = zbyrank(&z, r);
            check(b && b->member == m, "member at rank");
        }
    }
    int n = build_ref();
    check(n == z.len, "length");
    int i = 0;
    for (Node *x = z.head->lv[0].fwd; x; x = x->lv[0].fwd, i++) {
        check(x->member == order_ref[i], "forward order");
        check(x->back == (i ? by_member[order_ref[i - 1]] : NULL), "back pointer");
    }
    i = n - 1;
    for (Node *x = z.tail; x; x = x->back, i--) check(x->member == order_ref[i], "backward order");
    check(i == -1, "backward covers all");
    printf("added=%d rescored=%d removed=%d size=%d height=%d\n", adds, updates, removes, z.len, z.level);
    printf("lowest 5 (score:member):");
    Node *x = z.head->lv[0].fwd;
    for (int k = 0; k < 5 && x; k++, x = x->lv[0].fwd) printf(" %d:%d", x->score, x->member);
    printf("\nhighest 5 (reverse walk):");
    x = z.tail;
    for (int k = 0; k < 5 && x; k++, x = x->back) printf(" %d:%d", x->score, x->member);
    printf("\nmedian rank %d -> member %d\n", n / 2 + 1, zbyrank(&z, n / 2 + 1)->member);
    for (int k = 0; k < M; k++) if (by_member[k]) zdel(&z, by_member[k]);
    check(z.len == 0, "emptied");
    free(z.head);
    return 0;
}
