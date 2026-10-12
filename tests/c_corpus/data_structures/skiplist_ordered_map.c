/*
 * title: Skip list ordered map with seeded level generator
 * topic: data_structures
 * covers: skip list, geometric levels, update vector, insert/erase/find, lower bound, range scan, level histogram, sorted-array oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLVL 12
#define KEYS 1000

static unsigned long long rs = 0x5C1B115717ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct Node { int key, val, lvl; struct Node *next[]; } Node;
typedef struct { Node *head; int level, count; } Skip;

static Node *node_new(int key, int val, int lvl) {
    Node *n = malloc(sizeof(Node) + (size_t)lvl * sizeof(Node *));
    n->key = key; n->val = val; n->lvl = lvl;
    for (int i = 0; i < lvl; i++) n->next[i] = NULL;
    return n;
}
static void sl_init(Skip *s) { s->head = node_new(-1, 0, MAXLVL); s->level = 1; s->count = 0; }
static int rand_level(void) {
    int l = 1;
    while (l < MAXLVL && (rnd() & 3u) == 0) l++;   /* p = 1/4 */
    return l;
}
static Node *sl_lower_bound(const Skip *s, int key, Node **upd) {
    Node *x = s->head;
    for (int i = s->level - 1; i >= 0; i--) {
        while (x->next[i] && x->next[i]->key < key) x = x->next[i];
        if (upd) upd[i] = x;
    }
    return x->next[0];
}
static Node *sl_find(const Skip *s, int key) {
    Node *n = sl_lower_bound(s, key, NULL);
    return n && n->key == key ? n : NULL;
}
static int sl_put(Skip *s, int key, int val) {   /* returns 1 if inserted, 0 if updated */
    Node *upd[MAXLVL];
    Node *n = sl_lower_bound(s, key, upd);
    if (n && n->key == key) { n->val = val; return 0; }
    int lvl = rand_level();
    if (lvl > s->level) { for (int i = s->level; i < lvl; i++) upd[i] = s->head; s->level = lvl; }
    Node *m = node_new(key, val, lvl);
    for (int i = 0; i < lvl; i++) { m->next[i] = upd[i]->next[i]; upd[i]->next[i] = m; }
    s->count++;
    return 1;
}
static int sl_erase(Skip *s, int key) {
    Node *upd[MAXLVL];
    Node *n = sl_lower_bound(s, key, upd);
    if (!n || n->key != key) return 0;
    for (int i = 0; i < s->level; i++) if (upd[i]->next[i] == n) upd[i]->next[i] = n->next[i];
    free(n);
    while (s->level > 1 && !s->head->next[s->level - 1]) s->level--;
    s->count--;
    return 1;
}
static void sl_free(Skip *s) {
    Node *x = s->head;
    while (x) { Node *n = x->next[0]; free(x); x = n; }
}

/* oracle: array indexed by key */
static int present[KEYS], value[KEYS];

int main(void) {
    Skip s; sl_init(&s);
    int ins = 0, upd = 0, del = 0, miss = 0, found = 0;
    for (int step = 0; step < 20000; step++) {
        unsigned op = rnd() % 10;
        int k = (int)(rnd() % KEYS);
        if (op < 4) {
            int v = (int)(rnd() % 100000);
            int r = sl_put(&s, k, v);
            check(r == !present[k], "put result");
            if (r) ins++; else upd++;
            present[k] = 1; value[k] = v;
        } else if (op < 6) {
            int r = sl_erase(&s, k);
            check(r == present[k], "erase result");
            if (r) del++; else miss++;
            present[k] = 0;
        } else if (op < 8) {
            Node *n = sl_find(&s, k);
            check((n != NULL) == present[k], "find presence");
            if (n) { check(n->val == value[k], "find value"); found++; }
        } else if (op < 9) {
            Node *n = sl_lower_bound(&s, k, NULL);
            int j = k; while (j < KEYS && !present[j]) j++;
            check((j == KEYS && !n) || (n && n->key == j), "lower bound");
        } else {
            int hi = k + 60; long sum = 0, ref = 0; int c = 0, rc = 0;
            for (Node *n = sl_lower_bound(&s, k, NULL); n && n->key < hi; n = n->next[0]) { sum += n->val; c++; }
            for (int j = k; j < hi && j < KEYS; j++) if (present[j]) { ref += value[j]; rc++; }
            check(sum == ref && c == rc, "range sum");
        }
    }
    int total = 0, prev = -1;
    for (Node *n = s.head->next[0]; n; n = n->next[0]) { check(n->key > prev, "strictly sorted"); prev = n->key; total++; }
    int expect = 0; for (int i = 0; i < KEYS; i++) expect += present[i];
    check(total == expect && total == s.count, "count");
    int hist[MAXLVL + 1]; memset(hist, 0, sizeof hist);
    for (Node *n = s.head->next[0]; n; n = n->next[0]) hist[n->lvl]++;
    printf("inserted=%d updated=%d deleted=%d erase_miss=%d hits=%d\n", ins, upd, del, miss, found);
    printf("size=%d height=%d level histogram:", s.count, s.level);
    for (int l = 1; l <= s.level; l++) printf(" %d", hist[l]);
    printf("\n");
    /* check the geometric shape: level-1 nodes should be about 3/4 */
    printf("share of level-1 nodes: %d%%\n", hist[1] * 100 / (total ? total : 1));
    sl_free(&s);
    return 0;
}
