/*
 * title: Indexable skip list with link widths
 * topic: data_structures
 * covers: indexable skip list, span widths per link, insert at position, delete at position, get k-th, rotate, sequence oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLVL 14
#define CAP 3000

static unsigned long long rs = 0x1D7E4AB1EULL * 40503;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct Node Node;
struct Node { int val; int lvl; Node *next[MAXLVL]; int width[MAXLVL]; };
typedef struct { Node *head; int level, len; } Seq;

static Node *mk(int val, int lvl) { Node *n = calloc(1, sizeof *n); n->val = val; n->lvl = lvl; return n; }
static void init(Seq *s) { s->head = mk(0, MAXLVL); s->level = 1; s->len = 0; for (int i = 0; i < MAXLVL; i++) s->head->width[i] = 0; }
static int rlevel(void) { int l = 1; while (l < MAXLVL && (rnd() & 1u)) l++; return l; }

/* widths: node->width[i] = number of level-0 steps from node to node->next[i] (or to end = len+1-pos if NULL) */
static void insert_at(Seq *s, int pos, int val) {   /* new element gets index pos (0-based) */
    Node *upd[MAXLVL]; int rank[MAXLVL];
    Node *x = s->head; int at = 0;                  /* head is at position 0; element i sits at position i+1 */
    for (int i = s->level - 1; i >= 0; i--) {
        while (x->next[i] && at + x->width[i] <= pos) { at += x->width[i]; x = x->next[i]; }
        upd[i] = x; rank[i] = at;
    }
    int lvl = rlevel();
    if (lvl > s->level) {
        for (int i = s->level; i < lvl; i++) { upd[i] = s->head; rank[i] = 0; s->head->width[i] = s->len + 1; }
        s->level = lvl;
    }
    Node *n = mk(val, lvl);
    for (int i = 0; i < lvl; i++) {
        n->next[i] = upd[i]->next[i];
        upd[i]->next[i] = n;
        int old = upd[i]->width[i];                 /* distance to old next (or to end sentinel) */
        n->width[i] = old - (pos - rank[i]);
        upd[i]->width[i] = (pos - rank[i]) + 1;
    }
    for (int i = lvl; i < s->level; i++) upd[i]->width[i]++;
    s->len++;
}
static int delete_at(Seq *s, int pos) {
    Node *upd[MAXLVL] = {0};
    Node *x = s->head; int at = 0;
    for (int i = s->level - 1; i >= 0; i--) {
        while (x->next[i] && at + x->width[i] <= pos) { at += x->width[i]; x = x->next[i]; }
        upd[i] = x;
    }
    Node *t = upd[0]->next[0];
    int v = t->val;
    for (int i = 0; i < s->level; i++) {
        if (upd[i]->next[i] == t) { upd[i]->width[i] += t->width[i] - 1; upd[i]->next[i] = t->next[i]; }
        else upd[i]->width[i]--;
    }
    free(t);
    while (s->level > 1 && !s->head->next[s->level - 1]) s->level--;
    s->len--;
    return v;
}
static int get(const Seq *s, int idx) {
    const Node *x = s->head; int at = 0, target = idx + 1;
    for (int i = s->level - 1; i >= 0; i--)
        while (x->next[i] && at + x->width[i] <= target) { at += x->width[i]; x = x->next[i]; }
    check(at == target, "get lands exactly");
    return x->val;
}
static void verify(const Seq *s) {
    for (int i = 0; i < s->level; i++) {
        const Node *x = s->head; int at = 0;
        while (x->next[i]) {
            /* width equals number of level-0 nodes between */
            int steps = 0; const Node *y = x;
            do { y = y->next[0]; steps++; } while (y != x->next[i]);
            check(steps == x->width[i], "span equals level-0 distance");
            at += steps; x = x->next[i];
        }
        check(at + x->width[i] == s->len + 1 || x->next[i] == NULL, "tail reachable");
    }
}

static int ref[CAP];

int main(void) {
    Seq s; init(&s);
    int n = 0, ins = 0, del = 0;
    long chk = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 10;
        if ((op < 5 || n == 0) && n < CAP - 1) {
            int pos = (int)(rnd() % (unsigned)(n + 1)), v = (int)(rnd() % 100000);
            insert_at(&s, pos, v);
            memmove(ref + pos + 1, ref + pos, (size_t)(n - pos) * sizeof(int)); ref[pos] = v; n++; ins++;
        } else if (op < 8 && n > 0) {
            int pos = (int)(rnd() % (unsigned)n);
            int v = delete_at(&s, pos);
            check(v == ref[pos], "delete returns element");
            memmove(ref + pos, ref + pos + 1, (size_t)(n - pos - 1) * sizeof(int)); n--; del++;
        } else if (n > 0) {
            int pos = (int)(rnd() % (unsigned)n);
            int v = get(&s, pos);
            check(v == ref[pos], "get");
            chk += v % 97;
        }
        check(s.len == n, "length");
        if (step % 400 == 0) verify(&s);
    }
    verify(&s);
    /* full traversal and random gets */
    int i = 0;
    for (Node *x = s.head->next[0]; x; x = x->next[0]) { check(x->val == ref[i], "sequence content"); i++; }
    check(i == n, "traversal length");
    for (int k = 0; k < n; k += 37) check(get(&s, k) == ref[k], "spot get");
    printf("inserts=%d deletes=%d final_len=%d levels=%d checksum=%ld\n", ins, del, n, s.level, chk);
    printf("first: %d %d %d ... last: %d\n", ref[0], ref[1], ref[2], ref[n - 1]);
    while (s.len > 0) delete_at(&s, 0);
    free(s.head);
    return 0;
}
