/*
 * title: Smallest range covering elements from k sorted lists
 * topic: algorithms
 * covers: k-way pointer advance, min-heap of list heads, running maximum, sliding window over merged stream, tie-breaking
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 999u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}
static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

typedef struct {
    int val, list, idx;
} Node;

static int node_less(const Node *a, const Node *b) {
    if (a->val != b->val)
        return a->val < b->val;
    return a->list < b->list;
}

static void sift_down(Node *h, int n, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && node_less(&h[l], &h[m]))
            m = l;
        if (r < n && node_less(&h[r], &h[m]))
            m = r;
        if (m == i)
            return;
        Node t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

enum { MAXK = 8, MAXL = 40 };

/* smallest [lo,hi] containing at least one value of every list; smallest lo on ties */
static void smallest_range(int lists[][MAXL], const int *len, int k, int *lo, int *hi) {
    Node heap[MAXK];
    int mx = -2000000000;
    for (int i = 0; i < k; i++) {
        heap[i] = (Node){lists[i][0], i, 0};
        if (lists[i][0] > mx)
            mx = lists[i][0];
    }
    for (int i = k / 2 - 1; i >= 0; i--)
        sift_down(heap, k, i);
    *lo = heap[0].val;
    *hi = mx;
    for (;;) {
        Node top = heap[0];
        if (top.idx + 1 >= len[top.list])
            return;
        int nv = lists[top.list][top.idx + 1];
        heap[0] = (Node){nv, top.list, top.idx + 1};
        if (nv > mx)
            mx = nv;
        sift_down(heap, k, 0);
        int cl = heap[0].val;
        if (mx - cl < *hi - *lo) {
            *lo = cl;
            *hi = mx;
        }
    }
}

/* brute force: merge everything tagged, try every window start */
static void brute(int lists[][MAXL], const int *len, int k, int *lo, int *hi) {
    int best = 2000000000;
    *lo = *hi = 0;
    for (int i = 0; i < k; i++)
        for (int p = 0; p < len[i]; p++) {
            int start = lists[i][p], end = start;
            int ok = 1;
            for (int j = 0; j < k && ok; j++) {
                int found = 0, cand = 0;
                for (int q = 0; q < len[j]; q++)
                    if (lists[j][q] >= start) {
                        cand = lists[j][q];
                        found = 1;
                        break;
                    }
                if (!found)
                    ok = 0;
                else if (cand > end)
                    end = cand;
            }
            if (ok && (end - start < best || (end - start == best && start < *lo))) {
                best = end - start;
                *lo = start;
                *hi = end;
            }
        }
}

int main(void) {
    static int lists[MAXK][MAXL];
    int len[MAXK];
    for (int trial = 0; trial < 300; trial++) {
        int k = 2 + (int)(rnd() % (MAXK - 1));
        int range = 10 + (int)(rnd() % 200);
        for (int i = 0; i < k; i++) {
            len[i] = 1 + (int)(rnd() % MAXL);
            for (int j = 0; j < len[i]; j++)
                lists[i][j] = (int)(rnd() % (unsigned)range);
            qsort(lists[i], (size_t)len[i], sizeof(int), cmp_int);
        }
        int l1, h1, l2, h2;
        smallest_range(lists, len, k, &l1, &h1);
        brute(lists, len, k, &l2, &h2);
        if (h1 - l1 != h2 - l2)
            fail("range width");
        /* the reported window must really cover every list */
        for (int i = 0; i < k; i++) {
            int has = 0;
            for (int j = 0; j < len[i]; j++)
                has |= lists[i][j] >= l1 && lists[i][j] <= h1;
            if (!has)
                fail("coverage");
        }
    }
    printf("300 random instances verified\n");

    int demo[MAXK][MAXL] = {{4, 10, 15, 24, 26}, {0, 9, 12, 20}, {5, 18, 22, 30}};
    int dl[] = {5, 4, 4};
    int lo, hi;
    smallest_range(demo, dl, 3, &lo, &hi);
    printf("classic: [%d, %d] width %d\n", lo, hi, hi - lo);
    int demo2[MAXK][MAXL] = {{1, 2, 3}, {1, 2, 3}, {1, 2, 3}};
    int dl2[] = {3, 3, 3};
    smallest_range(demo2, dl2, 3, &lo, &hi);
    printf("identical lists: [%d, %d]\n", lo, hi);
    int demo3[MAXK][MAXL] = {{1}, {100}, {50}};
    int dl3[] = {1, 1, 1};
    smallest_range(demo3, dl3, 3, &lo, &hi);
    printf("singletons: [%d, %d]\n", lo, hi);
    return 0;
}
