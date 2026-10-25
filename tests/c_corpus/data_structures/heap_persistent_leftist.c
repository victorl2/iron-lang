/*
 * title: Persistent leftist heap with version history
 * topic: data_structures
 * covers: persistent data structure, path copying, structural sharing, leftist heap, versions, arena allocation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x5EED5EEDull;
static unsigned rng(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Nodes live in an arena and are never modified after creation; 0 is the empty heap. */
typedef struct {
    int key, npl;
    int l, r;
} PNode;

enum { ARENA = 200000, NV = 500 };
static PNode arena[ARENA];
static int arena_n = 1;
static int fresh; /* nodes created by the current operation */

static int npl(int x) { return x ? arena[x].npl : -1; }

static int mk(int key, int a, int b) {
    check(arena_n < ARENA, "arena capacity");
    if (npl(a) < npl(b)) {
        int t = a;
        a = b;
        b = t;
    }
    PNode *n = &arena[arena_n];
    n->key = key;
    n->l = a;
    n->r = b;
    n->npl = npl(b) + 1;
    fresh++;
    return arena_n++;
}

static int meld(int a, int b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (arena[b].key < arena[a].key) {
        int t = a;
        a = b;
        b = t;
    }
    return mk(arena[a].key, arena[a].l, meld(arena[a].r, b));
}

static int insert(int h, int key) { return meld(h, mk(key, 0, 0)); }

static int delete_min(int h, int *key) {
    *key = arena[h].key;
    return meld(arena[h].l, arena[h].r);
}

static int size_of(int h) { return h ? 1 + size_of(arena[h].l) + size_of(arena[h].r) : 0; }

static void collect(int h, int *out, int *n) {
    if (!h)
        return;
    out[(*n)++] = arena[h].key;
    collect(arena[h].l, out, n);
    collect(arena[h].r, out, n);
}

static void verify(int h) {
    if (!h)
        return;
    PNode *x = &arena[h];
    check(!x->l || arena[x->l].key >= x->key, "left order");
    check(!x->r || arena[x->r].key >= x->key, "right order");
    check(npl(x->l) >= npl(x->r), "leftist");
    check(x->npl == npl(x->r) + 1, "npl");
    verify(x->l);
    verify(x->r);
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static int floor_log2(int n) {
    int k = 0;
    while (n > 1) {
        n >>= 1;
        k++;
    }
    return k;
}

int main(void) {
    static int ver[NV];
    static int *model[NV];
    static int msz[NV];
    int nv = 1, maxfresh = 0;
    ver[0] = 0;
    model[0] = malloc(sizeof(int));
    msz[0] = 0;
    int ins = 0, pops = 0, melds = 0;
    while (nv < NV) {
        int r = (int)(rng() % 100);
        int a = rng() % 4 ? nv - 1 - (int)(rng() % (unsigned)(nv < 6 ? nv : 6)) : (int)(rng() % (unsigned)nv);
        fresh = 0;
        int h, *m, n;
        if (r < 68 || msz[a] == 0) {
            int k = (int)(rng() % 10000);
            h = insert(ver[a], k);
            n = msz[a] + 1;
            m = malloc(sizeof(int) * (size_t)n);
            memcpy(m, model[a], sizeof(int) * (size_t)msz[a]);
            m[n - 1] = k;
            check(fresh <= 2 * floor_log2(n + 1) + 3, "path copying creates O(log n) nodes");
            ins++;
        } else if (r < 80 - 0) {
            int k;
            h = delete_min(ver[a], &k);
            int *mm = model[a];
            int bi = 0;
            for (int i = 1; i < msz[a]; i++)
                if (mm[i] < mm[bi])
                    bi = i;
            check(k == mm[bi], "delete-min equals model minimum");
            n = msz[a] - 1;
            m = malloc(sizeof(int) * (size_t)(n + 1));
            for (int i = 0, j = 0; i < msz[a]; i++)
                if (i != bi)
                    m[j++] = mm[i];
            pops++;
        } else {
            int b = (int)(rng() % (unsigned)nv);
            h = meld(ver[a], ver[b]);
            n = msz[a] + msz[b];
            m = malloc(sizeof(int) * (size_t)(n + 1));
            memcpy(m, model[a], sizeof(int) * (size_t)msz[a]);
            memcpy(m + msz[a], model[b], sizeof(int) * (size_t)msz[b]);
            melds++;
        }
        if (fresh > maxfresh)
            maxfresh = fresh;
        ver[nv] = h;
        model[nv] = m;
        msz[nv] = n;
        nv++;
    }
    /* every version, including the oldest, must still hold exactly its model */
    long total_sz = 0;
    int biggest = 0;
    for (int v = 0; v < nv; v++) {
        verify(ver[v]);
        int n = 0;
        int *out = malloc(sizeof(int) * (size_t)(msz[v] + 1));
        collect(ver[v], out, &n);
        check(n == msz[v] && size_of(ver[v]) == n, "version size");
        qsort(out, (size_t)n, sizeof(int), cmp_int);
        qsort(model[v], (size_t)n, sizeof(int), cmp_int);
        check(memcmp(out, model[v], sizeof(int) * (size_t)n) == 0, "version content unchanged");
        /* draining a version creates new nodes but must not disturb it */
        int h = ver[v], k, prev = -1, c = 0;
        while (h) {
            h = delete_min(h, &k);
            check(k >= prev, "drain ascending");
            prev = k;
            c++;
        }
        check(c == n, "drain count");
        total_sz += n;
        if (n > biggest)
            biggest = n;
        free(out);
    }
    for (int v = 0; v < nv; v++) {
        int n = 0;
        int *out = malloc(sizeof(int) * (size_t)(msz[v] + 1));
        collect(ver[v], out, &n);
        qsort(out, (size_t)n, sizeof(int), cmp_int);
        check(n == msz[v] && memcmp(out, model[v], sizeof(int) * (size_t)n) == 0, "versions intact after draining all");
        free(out);
        free(model[v]);
    }
    printf("versions=%d inserts=%d pops=%d melds=%d\n", nv, ins, pops, melds);
    printf("largest=%d total_items_over_versions=%ld max_fresh_nodes_per_op=%d\n", biggest, total_sz, maxfresh);
    return 0;
}
