/*
 * title: Binomial heap with meld, decrease-key and delete
 * topic: data_structures
 * covers: binomial heap, binomial trees, union of root lists, decrease-key by handle, delete, invariant checks
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 20240607;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 20);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct Node {
    int key, id, degree;
    struct Node *parent, *child, *sibling;
} Node;

enum { MAXID = 3600, NEG_INF = -1000000 };
static Node *where[MAXID];

static Node *link_trees(Node *a, Node *b) { /* both same degree; returns root */
    if (b->key < a->key) {
        Node *t = a;
        a = b;
        b = t;
    }
    b->parent = a;
    b->sibling = a->child;
    a->child = b;
    a->degree++;
    return a;
}

/* merge two root lists (ascending degree) into one, no linking yet */
static Node *merge_roots(Node *a, Node *b) {
    Node head = {0, 0, 0, 0, 0, 0}, *t = &head;
    while (a && b) {
        if (a->degree <= b->degree) {
            t->sibling = a;
            a = a->sibling;
        } else {
            t->sibling = b;
            b = b->sibling;
        }
        t = t->sibling;
    }
    t->sibling = a ? a : b;
    return head.sibling;
}

static Node *bh_union(Node *a, Node *b) {
    Node *h = merge_roots(a, b);
    if (!h)
        return NULL;
    Node *prev = NULL, *x = h, *next = x->sibling;
    while (next) {
        if (x->degree != next->degree || (next->sibling && next->sibling->degree == x->degree)) {
            prev = x;
            x = next;
        } else if (x->key <= next->key) {
            x->sibling = next->sibling;
            link_trees(x, next);
        } else {
            if (prev)
                prev->sibling = next;
            else
                h = next;
            link_trees(next, x);
            x = next;
        }
        next = x->sibling;
    }
    return h;
}

static Node *bh_insert(Node *h, int key, int id) {
    Node *n = calloc(1, sizeof(Node));
    n->key = key;
    n->id = id;
    where[id] = n;
    return bh_union(h, n);
}

static Node *bh_extract_min(Node *h, int *key, int *id) {
    Node *best = h, *bp = NULL, *p = NULL;
    for (Node *x = h; x; p = x, x = x->sibling)
        if (x->key < best->key) {
            best = x;
            bp = p;
        }
    if (bp)
        bp->sibling = best->sibling;
    else
        h = best->sibling;
    Node *rev = NULL, *c = best->child;
    while (c) {
        Node *nx = c->sibling;
        c->sibling = rev;
        c->parent = NULL;
        rev = c;
        c = nx;
    }
    *key = best->key;
    *id = best->id;
    where[best->id] = NULL;
    free(best);
    return bh_union(h, rev);
}

static void swap_payload(Node *a, Node *b) {
    int k = a->key, i = a->id;
    a->key = b->key;
    a->id = b->id;
    b->key = k;
    b->id = i;
    where[a->id] = a;
    where[b->id] = b;
}

static void bh_decrease(Node *x, int nk) {
    x->key = nk;
    while (x->parent && x->key < x->parent->key) {
        swap_payload(x, x->parent);
        x = x->parent;
    }
}

static long tree_check(const Node *x, int deg, const Node *parent) {
    check(x->degree == deg && x->parent == parent, "tree shape");
    long size = 1;
    int want = deg - 1;
    for (const Node *c = x->child; c; c = c->sibling, want--) {
        check(c->key >= x->key, "heap order");
        check(where[c->id] == c, "handle");
        size += tree_check(c, want, x);
    }
    check(want == -1, "child count equals degree");
    return size;
}

static long bh_check(const Node *h) {
    long total = 0;
    int last = -1;
    for (const Node *x = h; x; x = x->sibling) {
        check(x->degree > last, "root degrees strictly ascending");
        last = x->degree;
        check(where[x->id] == x, "root handle");
        long s = tree_check(x, x->degree, NULL);
        check(s == (1L << x->degree), "binomial tree size");
        total += s;
    }
    return total;
}

static void destroy(Node *x) {
    while (x) {
        Node *nx = x->sibling;
        destroy(x->child);
        free(x);
        x = nx;
    }
}

int main(void) {
    Node *H[2] = {NULL, NULL};
    int state[MAXID] = {0}, key[MAXID] = {0}, next_id = 0;
    long count[2] = {0, 0};
    int melds = 0, dels = 0, decs = 0, pops = 0;
    long popsum = 0;
    for (int op = 0; op < 3500; op++) {
        int r = (int)(rng() % 100), hs = (int)(rng() % 2);
        if ((r < 50 || count[hs] == 0) && next_id < MAXID - 1) {
            int k = (int)(rng() % 10000);
            key[next_id] = k;
            state[next_id] = hs + 1;
            H[hs] = bh_insert(H[hs], k, next_id++);
            count[hs]++;
        } else if (r < 62 && count[hs] > 0) {
            int mk = 1 << 30;
            for (int i = 0; i < next_id; i++)
                if (state[i] == hs + 1 && key[i] < mk)
                    mk = key[i];
            int k, id;
            H[hs] = bh_extract_min(H[hs], &k, &id);
            check(k == mk && state[id] == hs + 1, "extract-min matches model");
            state[id] = 0;
            count[hs]--;
            pops++;
            popsum += k;
        } else if (r < 80 && count[hs] > 0) {
            int id = (int)(rng() % (unsigned)next_id);
            while (state[id] != hs + 1)
                id = (id + 1) % next_id;
            int nk = key[id] - (int)(rng() % 500);
            key[id] = nk;
            bh_decrease(where[id], nk);
            decs++;
        } else if (r < 88 && count[hs] > 0) {
            int id = (int)(rng() % (unsigned)next_id);
            while (state[id] != hs + 1)
                id = (id + 1) % next_id;
            bh_decrease(where[id], NEG_INF);
            int k, got;
            H[hs] = bh_extract_min(H[hs], &k, &got);
            check(got == id && k == NEG_INF, "delete removes the chosen id");
            state[id] = 0;
            count[hs]--;
            dels++;
        } else if (r >= 97) {
            H[0] = bh_union(H[0], H[1]);
            H[1] = NULL;
            for (int i = 0; i < next_id; i++)
                if (state[i] == 2)
                    state[i] = 1;
            count[0] += count[1];
            count[1] = 0;
            melds++;
        }
        for (int h = 0; h < 2; h++)
            check(bh_check(H[h]) == count[h], "size matches count");
    }
    H[0] = bh_union(H[0], H[1]);
    H[1] = NULL;
    long total = bh_check(H[0]);
    int shape = 0;
    for (Node *x = H[0]; x; x = x->sibling)
        shape |= 1 << x->degree;
    printf("inserted=%d pops=%d decrease=%d deletes=%d melds=%d\n", next_id, pops, decs, dels, melds);
    printf("popsum=%ld final=%ld root_degree_mask=0x%x\n", popsum, total, shape);
    check(shape == total, "root degree mask equals binary size");
    int prev = -(1 << 30), k, id, n = 0;
    while (H[0]) {
        H[0] = bh_extract_min(H[0], &k, &id);
        check(k >= prev, "drain ascending");
        prev = k;
        n++;
    }
    check(n == total, "drained everything");
    destroy(H[0]);
    return 0;
}
