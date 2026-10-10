/*
 * title: Rope as a B-tree of small text chunks
 * topic: data_structures
 * covers: rope, B-tree node splitting, cached subtree lengths, range delete, index and substring, bulk load
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 60606u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define LEAF_MAX 16
#define FAN 4

typedef struct Node Node;
struct Node {
    int leaf;
    int n;                    /* leaf: chars, internal: children */
    int len;                  /* total chars below this node */
    char text[LEAF_MAX];      /* leaf payload */
    Node *kid[FAN + 1];       /* one spare slot for overflow before splitting */
};

static long live_nodes;
static Node *nn(int leaf) {
    Node *x = calloc(1, sizeof *x);
    CHECK(x);
    x->leaf = leaf;
    live_nodes++;
    return x;
}
static void free_tree(Node *t) {
    if (!t) return;
    if (!t->leaf) for (int i = 0; i < t->n; i++) free_tree(t->kid[i]);
    free(t);
    live_nodes--;
}
static void recompute(Node *t) {
    if (t->leaf) { t->len = t->n; return; }
    t->len = 0;
    for (int i = 0; i < t->n; i++) t->len += t->kid[i]->len;
}

/* insert string s at position pos inside subtree t; returns a new right sibling if t split, else NULL */
static Node *ins(Node *t, int pos, const char *s, int slen) {
    if (t->leaf) {
        /* build the combined text in a temporary buffer and re-chunk */
        char tmp[LEAF_MAX + 64];
        CHECK(slen <= 64);
        memcpy(tmp, t->text, (size_t)pos);
        memcpy(tmp + pos, s, (size_t)slen);
        memcpy(tmp + pos + slen, t->text + pos, (size_t)(t->n - pos));
        int total = t->n + slen;
        if (total <= LEAF_MAX) {
            memcpy(t->text, tmp, (size_t)total);
            t->n = total; t->len = total;
            return NULL;
        }
        int half = total / 2;
        memcpy(t->text, tmp, (size_t)half);
        t->n = t->len = half;
        Node *r = nn(1);
        int rest = total - half;
        if (rest > LEAF_MAX) { /* only possible for very large inserts, split in three by recursion */
            CHECK(0);
        }
        memcpy(r->text, tmp + half, (size_t)rest);
        r->n = r->len = rest;
        return r;
    }
    int i = 0;
    while (i < t->n - 1 && pos > t->kid[i]->len) { pos -= t->kid[i]->len; i++; }
    Node *r = ins(t->kid[i], pos, s, slen);
    if (r) {
        memmove(&t->kid[i + 2], &t->kid[i + 1], (size_t)(t->n - i - 1) * sizeof(Node *));
        t->kid[i + 1] = r;
        t->n++;
    }
    recompute(t);
    if (t->n <= FAN) return NULL;
    Node *sib = nn(0);
    int keep = (t->n + 1) / 2;
    sib->n = t->n - keep;
    memcpy(sib->kid, &t->kid[keep], (size_t)sib->n * sizeof(Node *));
    t->n = keep;
    recompute(t); recompute(sib);
    return sib;
}
/* delete [from, to) relative to t; returns 1 if t became empty */
static int del(Node *t, int from, int to) {
    if (from < 0) from = 0;
    if (to > t->len) to = t->len;
    if (from >= to) return 0;
    if (t->leaf) {
        memmove(t->text + from, t->text + to, (size_t)(t->n - to));
        t->n -= to - from;
        t->len = t->n;
        return t->n == 0;
    }
    int base = 0;
    for (int i = 0; i < t->n;) {
        Node *k = t->kid[i];
        int lo = from - base, hi = to - base;
        int klen = k->len;
        int empty = 0;
        if (hi > 0 && lo < klen) empty = del(k, lo, hi);
        base += klen;
        if (empty) {
            free_tree(k);
            memmove(&t->kid[i], &t->kid[i + 1], (size_t)(t->n - i - 1) * sizeof(Node *));
            t->n--;
            /* from/to stay in original coordinates, so base keeps the child's original length */
            continue;
        }
        i++;
    }
    recompute(t);
    return t->n == 0;
}

typedef struct { Node *root; } Rope;
static void rope_init(Rope *r) { r->root = nn(1); }
static void rope_insert(Rope *r, int pos, const char *s) {
    int slen = (int)strlen(s);
    while (slen > 0) { /* feed long strings in small pieces */
        int piece = slen > 12 ? 12 : slen;
        Node *sib = ins(r->root, pos, s, piece);
        if (sib) {
            Node *nr = nn(0);
            nr->kid[0] = r->root; nr->kid[1] = sib; nr->n = 2;
            recompute(nr);
            r->root = nr;
        }
        pos += piece; s += piece; slen -= piece;
    }
}
static void rope_delete(Rope *r, int from, int to) {
    del(r->root, from, to);
    /* collapse single-child roots and reset an emptied root to a fresh leaf */
    while (!r->root->leaf && r->root->n == 1) {
        Node *k = r->root->kid[0];
        free(r->root);
        live_nodes--;
        r->root = k;
    }
    if (!r->root->leaf && r->root->n == 0) { free(r->root); live_nodes--; r->root = nn(1); }
}
static char rope_at(const Rope *r, int pos) {
    const Node *t = r->root;
    while (!t->leaf) {
        int i = 0;
        while (pos >= t->kid[i]->len) { pos -= t->kid[i]->len; i++; }
        t = t->kid[i];
    }
    return t->text[pos];
}
static int flatten(const Node *t, char *out, int at) {
    if (t->leaf) { memcpy(out + at, t->text, (size_t)t->n); return at + t->n; }
    for (int i = 0; i < t->n; i++) at = flatten(t->kid[i], out, at);
    return at;
}
/* structural audit; returns depth of leaves */
static int audit(const Node *t, int is_root, int *leaves, int *fill) {
    if (t->leaf) {
        CHECK(t->n <= LEAF_MAX && t->len == t->n);
        (*leaves)++;
        *fill += t->n;
        return 1;
    }
    CHECK(t->n >= 1 && t->n <= FAN);
    if (!is_root) CHECK(t->n >= 1);
    int len = 0, depth = -1;
    for (int i = 0; i < t->n; i++) {
        len += t->kid[i]->len;
        int d = audit(t->kid[i], 0, leaves, fill);
        CHECK(depth < 0 || depth == d);
        depth = d;
    }
    CHECK(len == t->len);
    return depth + 1;
}
/* bulk load: chunk a string into leaves of `fillsz` chars and build parents bottom-up */
static Node *bulk(const char *s, int n) {
    Node *level[512];
    int cnt = 0;
    for (int i = 0; i < n || cnt == 0; i += 12) {
        Node *l = nn(1);
        int k = n - i < 12 ? n - i : 12;
        if (k < 0) k = 0;
        memcpy(l->text, s + i, (size_t)k);
        l->n = l->len = k;
        CHECK(cnt < 512);
        level[cnt++] = l;
        if (n == 0) break;
    }
    while (cnt > 1) {
        int m = 0;
        for (int i = 0; i < cnt; i += FAN) {
            Node *p = nn(0);
            p->n = cnt - i < FAN ? cnt - i : FAN;
            memcpy(p->kid, &level[i], (size_t)p->n * sizeof(Node *));
            recompute(p);
            level[m++] = p;
        }
        cnt = m;
    }
    return level[0];
}

#define MAXT 3000
int main(void) {
    Rope r;
    rope_init(&r);
    static char model[MAXT + 100];
    int mn = 0;
    long inserts = 0, deletes = 0;
    for (int step = 0; step < 4000; step++) {
        unsigned op = rnd() % 10;
        if (op < 6 && mn < MAXT - 40) {
            int pos = (int)(rnd() % (unsigned)(mn + 1));
            char s[32];
            int k = 1 + (int)(rnd() % 30);
            for (int i = 0; i < k; i++) s[i] = (char)('a' + rnd() % 26);
            s[k] = 0;
            rope_insert(&r, pos, s);
            memmove(model + pos + k, model + pos, (size_t)(mn - pos));
            memcpy(model + pos, s, (size_t)k);
            mn += k;
            inserts++;
        } else if (mn > 0) {
            int a = (int)(rnd() % (unsigned)mn), len = 1 + (int)(rnd() % 40);
            int b = a + len > mn ? mn : a + len;
            rope_delete(&r, a, b);
            memmove(model + a, model + b, (size_t)(mn - b));
            mn -= b - a;
            deletes++;
        }
        CHECK(r.root->len == mn);
        if (step % 100 == 0) {
            int lv = 0, fl = 0;
            audit(r.root, 1, &lv, &fl);
            CHECK(fl == mn);
            char *flat = malloc((size_t)mn + 1);
            CHECK(flat);
            CHECK(flatten(r.root, flat, 0) == mn);
            CHECK(memcmp(flat, model, (size_t)mn) == 0);
            free(flat);
            for (int p = 0; p < mn; p += 97) CHECK(rope_at(&r, p) == model[p]);
        }
    }
    int leaves = 0, fill = 0;
    int height = audit(r.root, 1, &leaves, &fill);
    printf("inserts=%ld deletes=%ld length=%d height=%d leaves=%d average fill %.2f of %d\n", inserts, deletes, mn, height, leaves,
           leaves ? (double)fill / leaves : 0.0, LEAF_MAX);
    /* substring digest through indexed reads */
    unsigned h = 17;
    for (int p = 0; p < mn; p++) h = h * 31u + (unsigned char)rope_at(&r, p);
    unsigned h2 = 17;
    for (int p = 0; p < mn; p++) h2 = h2 * 31u + (unsigned char)model[p];
    CHECK(h == h2);
    printf("content hash %u, first 20 chars: %.20s\n", h % 1000003u, model);
    /* compaction: flatten and bulk load a densely packed tree */
    char *flat = malloc((size_t)mn + 1);
    CHECK(flat);
    flatten(r.root, flat, 0);
    free_tree(r.root);
    r.root = bulk(flat, mn);
    free(flat);
    int l2 = 0, f2 = 0;
    int h_after = audit(r.root, 1, &l2, &f2);
    CHECK(f2 == mn && r.root->len == mn);
    for (int p = 0; p < mn; p += 13) CHECK(rope_at(&r, p) == model[p]);
    printf("after bulk reload: height=%d leaves=%d average fill %.2f\n", h_after, l2, l2 ? (double)f2 / l2 : 0.0);
    free_tree(r.root);
    CHECK(live_nodes == 0);
    return 0;
}
