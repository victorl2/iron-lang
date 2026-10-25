/*
 * title: Compressed radix tree with split and merge
 * topic: data_structures
 * covers: radix tree, edge splitting, edge merging on delete, sorted prefix listing, structural invariants
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define AL 4
typedef struct RNode {
    char *label; /* edge label leading into this node */
    struct RNode *kid[AL];
    int term;
} RNode;

static int nodes_live = 0;

static char *dup_n(const char *s, size_t n) {
    char *d = malloc(n + 1);
    check(d != NULL, "alloc");
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static RNode *rn_new(const char *lab, size_t n, int term) {
    RNode *r = calloc(1, sizeof *r);
    check(r != NULL, "alloc");
    r->label = dup_n(lab, n);
    r->term = term;
    nodes_live++;
    return r;
}

static int nkids(const RNode *n) {
    int c = 0;
    for (int i = 0; i < AL; i++)
        c += n->kid[i] != NULL;
    return c;
}

static size_t common(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return i;
}

/* returns 1 if newly inserted */
static int rt_insert(RNode *root, const char *w) {
    RNode *n = root;
    for (;;) {
        if (!*w) {
            int fresh = !n->term;
            n->term = 1;
            return fresh;
        }
        int c = *w - 'a';
        RNode *k = n->kid[c];
        if (!k) {
            n->kid[c] = rn_new(w, strlen(w), 1);
            return 1;
        }
        size_t l = common(k->label, w);
        size_t kl = strlen(k->label);
        if (l < kl) {
            /* split k at l */
            RNode *mid = rn_new(k->label, l, 0);
            char *rest = dup_n(k->label + l, kl - l);
            free(k->label);
            k->label = rest;
            mid->kid[rest[0] - 'a'] = k;
            n->kid[c] = mid;
            k = mid;
        }
        w += l;
        n = k;
    }
}

static const RNode *rt_find(const RNode *root, const char *w, size_t *rest_in_edge) {
    const RNode *n = root;
    *rest_in_edge = 0;
    while (*w) {
        const RNode *k = n->kid[*w - 'a'];
        if (!k)
            return NULL;
        size_t kl = strlen(k->label);
        size_t l = common(k->label, w);
        if (l == strlen(w)) {
            *rest_in_edge = kl - l; /* prefix ends inside or at end of edge */
            return k;
        }
        if (l < kl)
            return NULL;
        w += l;
        n = k;
    }
    return n;
}

static int rt_contains(const RNode *root, const char *w) {
    size_t rest;
    const RNode *n = rt_find(root, w, &rest);
    return n && rest == 0 && n->term;
}

static int rt_erase_rec(RNode *parent, int c, const char *w) {
    RNode *k = parent->kid[c];
    if (!k)
        return 0;
    size_t kl = strlen(k->label);
    if (strncmp(k->label, w, kl) != 0)
        return 0;
    w += kl;
    int removed;
    if (*w)
        removed = rt_erase_rec(k, *w - 'a', w);
    else {
        removed = k->term;
        k->term = 0;
    }
    if (!removed)
        return 0;
    int nk = nkids(k);
    if (!k->term && nk == 0) {
        parent->kid[c] = NULL;
        free(k->label);
        free(k);
        nodes_live--;
    } else if (!k->term && nk == 1) {
        RNode *only = NULL;
        for (int i = 0; i < AL; i++)
            if (k->kid[i])
                only = k->kid[i];
        size_t a = strlen(k->label), b = strlen(only->label);
        char *m = malloc(a + b + 1);
        check(m != NULL, "alloc");
        memcpy(m, k->label, a);
        memcpy(m + a, only->label, b + 1);
        free(only->label);
        only->label = m;
        parent->kid[c] = only;
        free(k->label);
        free(k);
        nodes_live--;
    }
    return 1;
}

static void collect(const RNode *n, char *buf, size_t len, char out[][16], int *cnt) {
    if (n->term) {
        buf[len] = 0;
        strcpy(out[(*cnt)++], buf);
    }
    for (int i = 0; i < AL; i++) {
        const RNode *k = n->kid[i];
        if (!k)
            continue;
        size_t kl = strlen(k->label);
        memcpy(buf + len, k->label, kl);
        collect(k, buf, len + kl, out, cnt);
    }
}

static int check_shape(const RNode *n, int is_root) {
    int cnt = n->term ? 1 : 0;
    if (!is_root) {
        check(strlen(n->label) > 0, "non-empty label");
        check(n->term || nkids(n) >= 2, "compressed");
    }
    for (int i = 0; i < AL; i++)
        if (n->kid[i]) {
            check(n->kid[i]->label[0] - 'a' == i, "slot matches first char");
            cnt += check_shape(n->kid[i], 0);
        }
    return cnt;
}

static void rt_free(RNode *n) {
    for (int i = 0; i < AL; i++)
        if (n->kid[i])
            rt_free(n->kid[i]);
    free(n->label);
    free(n);
    nodes_live--;
}

static char set[1400][16];
static int nset = 0;

static int set_find(const char *w) {
    for (int i = 0; i < nset; i++)
        if (strcmp(set[i], w) == 0)
            return i;
    return -1;
}

int main(void) {
    RNode *root = rn_new("", 0, 0);
    int ins = 0, del = 0;
    for (int step = 0; step < 5000; step++) {
        char w[16];
        int len = 1 + (int)rnd(5);
        for (int i = 0; i < len; i++)
            w[i] = (char)('a' + rnd(AL));
        w[len] = 0;
        unsigned op = rnd(10);
        if (op < 4) {
            int fresh = rt_insert(root, w);
            int idx = set_find(w);
            check(fresh == (idx < 0), "insert freshness");
            if (fresh) {
                strcpy(set[nset++], w);
                ins++;
            }
        } else if (op < 7) {
            int got = rt_erase_rec(root, w[0] - 'a', w);
            int idx = set_find(w);
            check(got == (idx >= 0), "erase result");
            if (got) {
                nset--;
                if (idx != nset)
                    strcpy(set[idx], set[nset]);
                del++;
            }
        } else {
            check(rt_contains(root, w) == (set_find(w) >= 0), "contains");
        }
        if (step % 250 == 0)
            check(check_shape(root, 1) == nset, "shape and count");
    }
    check(check_shape(root, 1) == nset, "final shape");
    char (*out)[16] = malloc(sizeof(char[16]) * 1400);
    check(out != NULL, "alloc");
    char buf[32];
    int cnt = 0;
    collect(root, buf, 0, out, &cnt);
    check(cnt == nset, "listing size");
    for (int i = 1; i < cnt; i++)
        check(strcmp(out[i - 1], out[i]) < 0, "sorted listing");
    printf("inserted=%d erased=%d live=%d nodes=%d\n", ins, del, nset, nodes_live);
    printf("first:");
    for (int i = 0; i < 6 && i < cnt; i++)
        printf(" %s", out[i]);
    printf("\nlast:");
    for (int i = cnt > 6 ? cnt - 6 : 0; i < cnt; i++)
        printf(" %s", out[i]);
    printf("\n");
    /* prefix listing */
    const char *prefs[] = {"ab", "dcd", "b", "cccc"};
    for (int p = 0; p < 4; p++) {
        int expect = 0;
        for (int i = 0; i < nset; i++)
            expect += strncmp(set[i], prefs[p], strlen(prefs[p])) == 0;
        size_t rest;
        const RNode *n = rt_find(root, prefs[p], &rest);
        int got = 0;
        if (n) {
            char pb[32];
            strcpy(pb, prefs[p]);
            size_t pl = strlen(pb);
            /* rest chars of edge label complete the prefix */
            size_t kl = strlen(n->label);
            memcpy(pb + pl, n->label + (kl - rest), rest);
            char (*o2)[16] = malloc(sizeof(char[16]) * 1400);
            check(o2 != NULL, "alloc");
            collect(n, pb, pl + rest, o2, &got);
            free(o2);
        }
        check(got == expect, "prefix listing");
        printf("prefix %s: %d\n", prefs[p], got);
    }
    free(out);
    rt_free(root);
    check(nodes_live == 0, "no leak");
    return 0;
}
