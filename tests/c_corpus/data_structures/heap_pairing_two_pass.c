/*
 * title: Pairing heap with two-pass delete-min and decrease-key
 * topic: data_structures
 * covers: pairing heap, two-pass pairing, subtree cut, decrease-key, meld, delete, invariant checks
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 31337;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 24);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* prev is the parent for a first child, otherwise the left sibling */
typedef struct PNode {
    int key, id;
    struct PNode *child, *next, *prev;
} PNode;

enum { MAXID = 4000, NEG_INF = -1000000 };
static PNode *where[MAXID];
static long links;

static PNode *meld(PNode *a, PNode *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (b->key < a->key) {
        PNode *t = a;
        a = b;
        b = t;
    }
    b->next = a->child;
    if (a->child)
        a->child->prev = b;
    b->prev = a;
    a->child = b;
    links++;
    return a;
}

static PNode *insert(PNode *h, int key, int id) {
    PNode *n = calloc(1, sizeof(PNode));
    n->key = key;
    n->id = id;
    where[id] = n;
    return meld(h, n);
}

static PNode *two_pass(PNode *first) {
    PNode *stack[MAXID];
    int cnt = 0;
    while (first) {
        PNode *a = first, *b = a->next;
        first = b ? b->next : NULL;
        a->next = a->prev = NULL;
        if (b)
            b->next = b->prev = NULL;
        stack[cnt++] = meld(a, b);
    }
    PNode *r = NULL;
    while (cnt > 0)
        r = meld(stack[--cnt], r);
    return r;
}

static PNode *extract_min(PNode *h, int *key, int *id) {
    PNode *c = h->child;
    *key = h->key;
    *id = h->id;
    where[h->id] = NULL;
    free(h);
    if (c)
        c->prev = NULL;
    return two_pass(c);
}

static PNode *cut(PNode *h, PNode *x) {
    if (x == h)
        return h;
    if (x->prev->child == x)
        x->prev->child = x->next;
    else
        x->prev->next = x->next;
    if (x->next)
        x->next->prev = x->prev;
    x->prev = x->next = NULL;
    return x;
}

static PNode *decrease(PNode *h, PNode *x, int nk) {
    check(nk <= x->key, "only decrease");
    x->key = nk;
    if (x == h)
        return h;
    /* find real parent: walk left over siblings */
    PNode *p = x;
    while (p->prev->child != p)
        p = p->prev;
    p = p->prev;
    if (x->key >= p->key)
        return h;
    cut(h, x);
    return meld(h, x);
}

static long count_check(const PNode *x, const PNode *parent) {
    long n = 0;
    const PNode *left = NULL;
    for (const PNode *c = x; c; left = c, c = c->next) {
        check(c->prev == (left ? left : parent), "prev link");
        check(!parent || c->key >= parent->key, "heap order");
        check(where[c->id] == c, "handle");
        n += 1 + count_check(c->child, c);
    }
    return n;
}

static long pcheck(const PNode *h) {
    if (!h)
        return 0;
    check(!h->prev && !h->next, "root links");
    return count_check(h, NULL);
}

static void destroy(PNode *x) {
    while (x) {
        PNode *n = x->next;
        destroy(x->child);
        free(x);
        x = n;
    }
}

int main(void) {
    PNode *H[2] = {NULL, NULL};
    int state[MAXID] = {0}, key[MAXID] = {0}, next_id = 0;
    long cnt[2] = {0, 0}, popsum = 0;
    int pops = 0, decs = 0, dels = 0, melds = 0;
    for (int op = 0; op < 3600; op++) {
        int r = (int)(rng() % 100), s = (int)(rng() % 2);
        if ((r < 48 || cnt[s] == 0) && next_id < MAXID) {
            int k = (int)(rng() % 30000);
            key[next_id] = k;
            state[next_id] = s + 1;
            H[s] = insert(H[s], k, next_id++);
            cnt[s]++;
        } else if (r < 62 && cnt[s]) {
            int mk = 1 << 30;
            for (int i = 0; i < next_id; i++)
                if (state[i] == s + 1 && key[i] < mk)
                    mk = key[i];
            int k, id;
            H[s] = extract_min(H[s], &k, &id);
            check(k == mk && state[id] == s + 1, "extract-min equals model");
            state[id] = 0;
            cnt[s]--;
            pops++;
            popsum += k;
        } else if (r < 85 && cnt[s]) {
            int id = (int)(rng() % (unsigned)next_id);
            while (state[id] != s + 1)
                id = (id + 1) % next_id;
            key[id] -= (int)(rng() % 4000);
            H[s] = decrease(H[s], where[id], key[id]);
            decs++;
        } else if (r < 92 && cnt[s]) {
            int id = (int)(rng() % (unsigned)next_id);
            while (state[id] != s + 1)
                id = (id + 1) % next_id;
            H[s] = decrease(H[s], where[id], NEG_INF);
            int k, got;
            H[s] = extract_min(H[s], &k, &got);
            check(got == id, "delete removes chosen id");
            state[id] = 0;
            cnt[s]--;
            dels++;
        } else if (r >= 97) {
            H[0] = meld(H[0], H[1]);
            H[1] = NULL;
            for (int i = 0; i < next_id; i++)
                if (state[i] == 2)
                    state[i] = 1;
            cnt[0] += cnt[1];
            cnt[1] = 0;
            melds++;
        }
        for (int h = 0; h < 2; h++)
            check(pcheck(H[h]) == cnt[h], "size");
    }
    printf("inserted=%d pops=%d decrease=%d deletes=%d melds=%d\n", next_id, pops, decs, dels, melds);
    printf("popsum=%ld live=%ld links=%ld\n", popsum, cnt[0] + cnt[1], links);
    H[0] = meld(H[0], H[1]);
    int prev = -(1 << 30), k, id;
    long n = 0;
    while (H[0]) {
        H[0] = extract_min(H[0], &k, &id);
        check(k >= prev, "drain ascending");
        prev = k;
        n++;
    }
    printf("drained=%ld\n", n);
    destroy(H[0]);
    return 0;
}
