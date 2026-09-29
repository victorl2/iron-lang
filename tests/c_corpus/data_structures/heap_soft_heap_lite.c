/*
 * title: Soft heap (Chazelle / Kaplan-Zwick style) with corruption accounting
 * topic: data_structures
 * covers: soft heap, approximate priority queue, item carpooling, corrupted keys, sift, binomial-style linking and meld
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 987654321ull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct Item {
    int key, id;
    struct Item *next;
} Item;

/* Every node keeps a list of items that all share the node's common key ckey (>= each item's own key). */
typedef struct SNode {
    int ckey, rank, cnt;
    Item *head, *tail;
    struct SNode *l, *r;
} SNode;

enum { MAXRANK = 40 };
typedef struct {
    SNode *root[MAXRANK];
    int sz[MAXRANK]; /* target list size per rank */
} Soft;

static void soft_init(Soft *s, int r) {
    memset(s->root, 0, sizeof s->root);
    for (int k = 0; k < MAXRANK; k++)
        s->sz[k] = k <= r ? 1 : (3 * s->sz[k - 1] + 1) / 2;
}

static void sift(const Soft *s, SNode *x) {
    while (x->cnt < s->sz[x->rank] && (x->l || x->r)) {
        if (!x->l || (x->r && x->l->ckey > x->r->ckey)) {
            SNode *t = x->l;
            x->l = x->r;
            x->r = t;
        }
        SNode *c = x->l;
        if (x->cnt == 0)
            x->head = c->head;
        else
            x->tail->next = c->head;
        x->tail = c->tail;
        x->cnt += c->cnt;
        x->ckey = c->ckey;
        c->head = c->tail = NULL;
        c->cnt = 0;
        if (!c->l && !c->r) {
            free(c);
            x->l = NULL;
        } else
            sift(s, c);
    }
}

static SNode *link_trees(const Soft *s, SNode *a, SNode *b) {
    SNode *z = calloc(1, sizeof(SNode));
    z->rank = a->rank + 1;
    z->l = a;
    z->r = b;
    sift(s, z);
    return z;
}

static void soft_meld(Soft *a, Soft *b) {
    SNode *carry = NULL;
    for (int k = 0; k < MAXRANK - 1; k++) {
        SNode *v[3];
        int m = 0;
        if (a->root[k])
            v[m++] = a->root[k];
        if (b->root[k])
            v[m++] = b->root[k];
        if (carry)
            v[m++] = carry;
        b->root[k] = NULL;
        carry = NULL;
        if (m == 0)
            a->root[k] = NULL;
        else if (m == 1)
            a->root[k] = v[0];
        else if (m == 2) {
            a->root[k] = NULL;
            carry = link_trees(a, v[0], v[1]);
        } else {
            a->root[k] = v[0];
            carry = link_trees(a, v[1], v[2]);
        }
    }
    check(carry == NULL, "rank overflow");
}

static void soft_insert(Soft *s, int key, int id) {
    Item *it = calloc(1, sizeof(Item));
    it->key = key;
    it->id = id;
    SNode *leaf = calloc(1, sizeof(SNode));
    leaf->ckey = key;
    leaf->head = leaf->tail = it;
    leaf->cnt = 1;
    Soft one;
    memset(one.root, 0, sizeof one.root);
    one.root[0] = leaf;
    soft_meld(s, &one);
}

/* returns the item (caller frees) and the common key it carried */
static Item *soft_delete_min(Soft *s, int *ckey) {
    int bk = -1;
    for (int k = 0; k < MAXRANK; k++)
        if (s->root[k] && (bk < 0 || s->root[k]->ckey < s->root[bk]->ckey))
            bk = k;
    SNode *x = s->root[bk];
    Item *it = x->head;
    *ckey = x->ckey;
    x->head = it->next;
    x->cnt--;
    if (!x->head)
        x->tail = NULL;
    it->next = NULL;
    if (x->cnt <= s->sz[bk] / 2) {
        if (x->l || x->r)
            sift(s, x);
        else if (x->cnt == 0) {
            free(x);
            s->root[bk] = NULL;
        }
    }
    return it;
}

/* invariant walk: fills ck[id] with the common key of each live item; returns item count */
static int walk(const SNode *x, const SNode *parent, int *ck, unsigned char *seen) {
    if (!x)
        return 0;
    check(x->cnt > 0, "nonempty item list");
    if (parent) {
        check(x->ckey >= parent->ckey, "common keys are heap ordered");
        check(x->rank == parent->rank - 1, "child rank");
    }
    int c = 0;
    for (const Item *i = x->head; i; i = i->next) {
        check(i->key <= x->ckey, "common key never below an item key");
        check(!seen[i->id], "item appears once");
        seen[i->id] = 1;
        ck[i->id] = x->ckey;
        c++;
    }
    check(c == x->cnt, "list length");
    check(!x->tail || !x->tail->next, "tail is last");
    return c + walk(x->l, x, ck, seen) + walk(x->r, x, ck, seen);
}

static int snapshot(const Soft *s, int *ck, unsigned char *seen, int nids) {
    memset(seen, 0, (size_t)nids);
    int t = 0;
    for (int k = 0; k < MAXRANK; k++)
        if (s->root[k]) {
            check(s->root[k]->rank == k, "root rank");
            t += walk(s->root[k], NULL, ck, seen);
        }
    return t;
}

static void destroy(SNode *x) {
    if (!x)
        return;
    while (x->head) {
        Item *n = x->head->next;
        free(x->head);
        x->head = n;
    }
    destroy(x->l);
    destroy(x->r);
    free(x);
}

int main(void) {
    enum { N = 1200 };
    int rs_list[] = {2, 3, 4, 6, 14};
    printf("%-3s %-5s %-9s %-9s %-11s\n", "r", "maxK", "peak_corr", "pop_corr", "exact_pops");
    for (size_t ri = 0; ri < sizeof rs_list / sizeof rs_list[0]; ri++) {
        int r = rs_list[ri];
        Soft A, B;
        soft_init(&A, r);
        soft_init(&B, r);
        static int key[2 * N], ck[2 * N];
        static unsigned char alive[2 * N], seen[2 * N];
        memset(alive, 0, sizeof alive);
        int ids = 0, inserted = 0, pops = 0, corrupt_pops = 0, exact_pops = 0, peak = 0, maxk = 0;
        int inB[2 * N];
        memset(inB, 0, sizeof inB);
        for (;;) {
            int did = 0;
            int r100 = (int)(rng() % 100);
            if (inserted < N && r100 < 64) {
                int useB = (rng() % 4 == 0);
                key[ids] = (int)(rng() % 100000);
                alive[ids] = 1;
                inB[ids] = useB;
                soft_insert(useB ? &B : &A, key[ids], ids);
                ids++;
                inserted++;
                did = 1;
            } else if (r100 >= 64 && r100 < 68) {
                soft_meld(&A, &B);
                for (int i = 0; i < ids; i++)
                    inB[i] = 0;
                did = 1;
            } else {
                int cnt = 0;
                for (int i = 0; i < ids; i++)
                    cnt += alive[i] && !inB[i];
                if (cnt > 0) {
                    int c;
                    Item *it = soft_delete_min(&A, &c);
                    check(alive[it->id] && !inB[it->id] && it->key <= c, "delete returns a live item of A");
                    alive[it->id] = 0;
                    pops++;
                    if (it->key == c)
                        exact_pops++;
                    else
                        corrupt_pops++;
                    /* soft guarantee: c <= common key of everything left in A */
                    int left = snapshot(&A, ck, seen, ids);
                    int n_alive = 0;
                    for (int i = 0; i < ids; i++)
                        if (alive[i] && !inB[i]) {
                            n_alive++;
                            check(seen[i] && ck[i] >= c, "returned common key is minimal");
                            if (ck[i] == key[i])
                                check(it->key <= key[i], "returned key <= every uncorrupted remaining key");
                        }
                    check(left == n_alive, "A holds exactly the live A items");
                    free(it);
                    did = 1;
                }
            }
            if (inserted == N && !did)
                break;
            if (inserted == N && pops > 0) {
                int cnt = 0;
                for (int i = 0; i < ids; i++)
                    cnt += alive[i];
                if (cnt == 0)
                    break;
                int a_cnt = 0;
                for (int i = 0; i < ids; i++)
                    a_cnt += alive[i] && !inB[i];
                if (a_cnt == 0) {
                    soft_meld(&A, &B);
                    for (int i = 0; i < ids; i++)
                        inB[i] = 0;
                }
            }
            /* corruption census on A */
            snapshot(&A, ck, seen, ids);
            int corr = 0;
            for (int i = 0; i < ids; i++)
                if (alive[i] && !inB[i] && seen[i] && ck[i] > key[i])
                    corr++;
            if (corr > peak)
                peak = corr;
            for (int k = 0; k < MAXRANK; k++)
                if (A.root[k] && k > maxk)
                    maxk = k;
        }
        check(peak * (1 << (r - 2)) <= inserted, "corruption stays within about n / 2^(r-2)");
        if (r >= 14)
            check(corrupt_pops == 0 && peak == 0, "large r behaves as an exact heap");
        printf("%-3d %-5d %-9d %-9d %-11d\n", r, maxk, peak, corrupt_pops, exact_pops);
        for (int k = 0; k < MAXRANK; k++) {
            destroy(A.root[k]);
            destroy(B.root[k]);
        }
    }
    return 0;
}
