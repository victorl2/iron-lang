/*
 * title: Segment tree of intervals with insert, remove and stabbing
 * topic: data_structures
 * covers: canonical node decomposition, per-node id lists, interval insertion and deletion, stabbing query root-to-leaf walk, brute force
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

#define M 512 /* coordinates [0, M) */

typedef struct {
    int *ids;
    int n, cap;
} List;

static List node[2 * M];
static long list_entries;

static void push(List *l, int id) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 2;
        l->ids = realloc(l->ids, sizeof(int) * (size_t)l->cap);
        check(l->ids != NULL, "alloc");
    }
    l->ids[l->n++] = id;
    list_entries++;
}

static int erase(List *l, int id) {
    for (int i = 0; i < l->n; i++)
        if (l->ids[i] == id) {
            l->ids[i] = l->ids[--l->n];
            list_entries--;
            return 1;
        }
    return 0;
}

/* half-open [lo, hi) */
static int insert_iv(int lo, int hi, int id) {
    int cnt = 0;
    for (lo += M, hi += M; lo < hi; lo >>= 1, hi >>= 1) {
        if (lo & 1) {
            push(&node[lo++], id);
            cnt++;
        }
        if (hi & 1) {
            push(&node[--hi], id);
            cnt++;
        }
    }
    return cnt;
}

static void remove_iv(int lo, int hi, int id) {
    for (lo += M, hi += M; lo < hi; lo >>= 1, hi >>= 1) {
        if (lo & 1)
            check(erase(&node[lo++], id), "remove canonical");
        if (hi & 1)
            check(erase(&node[--hi], id), "remove canonical");
    }
}

static int stab(int x, int *out) {
    int cnt = 0;
    for (int i = x + M; i >= 1; i >>= 1)
        for (int k = 0; k < node[i].n; k++)
            out[cnt++] = node[i].ids[k];
    return cnt;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int main(void) {
    enum { MAXI = 300 };
    static int lo[MAXI], hi[MAXI], alive[MAXI];
    int ni = 0, max_pieces = 0;
    long qsum = 0;
    int ins = 0, del = 0, nq = 0;
    for (int step = 0; step < 4000; step++) {
        unsigned op = rnd(6);
        if (op < 2 && ni < MAXI) {
            int a = (int)rnd(M), len = 1 + (int)rnd(200);
            int b = a + len > M ? M : a + len;
            lo[ni] = a;
            hi[ni] = b;
            alive[ni] = 1;
            int pieces = insert_iv(a, b, ni);
            if (pieces > max_pieces)
                max_pieces = pieces;
            ni++;
            ins++;
        } else if (op == 2 && ni > 0) {
            int id = (int)rnd((unsigned)ni);
            if (alive[id]) {
                remove_iv(lo[id], hi[id], id);
                alive[id] = 0;
                del++;
            }
        } else {
            int x = (int)rnd(M);
            int got[MAXI * 20];
            int cnt = stab(x, got);
            qsort(got, (size_t)cnt, sizeof(int), cmp_int);
            int want[MAXI], nw = 0;
            for (int i = 0; i < ni; i++)
                if (alive[i] && lo[i] <= x && x < hi[i])
                    want[nw++] = i;
            check(cnt == nw, "stab count");
            for (int i = 0; i < nw; i++)
                check(got[i] == want[i], "stab ids");
            qsum += cnt;
            nq++;
        }
    }
    printf("inserted=%d removed=%d stab queries=%d results=%ld\n", ins, del, nq, qsum);
    printf("max canonical pieces per interval=%d, stored entries=%ld\n", max_pieces, list_entries);
    /* remove everything: no entries may remain */
    for (int i = 0; i < ni; i++)
        if (alive[i])
            remove_iv(lo[i], hi[i], i);
    check(list_entries == 0, "empty after removing all");
    printf("entries after clearing=%ld\n", list_entries);
    for (int i = 0; i < 2 * M; i++)
        free(node[i].ids);
    return 0;
}
