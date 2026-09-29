/*
 * title: Union-find with circular member lists and component aggregates
 * topic: data_structures
 * covers: DSU, circular linked member ring, O(1) ring splice on union, component enumeration, min/max/sum per root, brute-force relabeling
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 80

static unsigned long long rs = 0xA66A6A7E5ULL * 53;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int par[N], sz[N], ring[N];         /* ring[x]: next member in x's circular list */
static long sum[N]; static int mn[N], mx[N]; /* valid at roots */
static int value[N];

static int find(int x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; }
static int unite(int a, int b) {
    a = find(a); b = find(b);
    if (a == b) return 0;
    if (sz[a] < sz[b]) { int t = a; a = b; b = t; }
    par[b] = a; sz[a] += sz[b]; sum[a] += sum[b];
    if (mn[b] < mn[a]) mn[a] = mn[b];
    if (mx[b] > mx[a]) mx[a] = mx[b];
    int t = ring[a]; ring[a] = ring[b]; ring[b] = t;   /* splice two rings in O(1) */
    return 1;
}

static int label[N];
static void relabel(const int *edges, int ne) { /* brute force: repeated propagation of min label */
    for (int i = 0; i < N; i++) label[i] = i;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < ne; i++) {
            int a = edges[2 * i], b = edges[2 * i + 1];
            if (label[a] < label[b]) { label[b] = label[a]; changed = 1; }
            else if (label[b] < label[a]) { label[a] = label[b]; changed = 1; }
        }
    }
}

int main(void) {
    for (int i = 0; i < N; i++) {
        par[i] = i; sz[i] = 1; ring[i] = i; value[i] = (int)(rnd() % 1000) - 300;
        sum[i] = mn[i] = mx[i] = value[i];
    }
    int edges[2 * 200], ne = 0;
    for (int round = 1; round <= 5; round++) {
        int batch = round == 1 ? 20 : 12;
        for (int i = 0; i < batch; i++) {
            int a = (int)(rnd() % N), b = (int)(rnd() % N);
            edges[2 * ne] = a; edges[2 * ne + 1] = b; ne++;
            unite(a, b);
        }
        relabel(edges, ne);
        int comps = 0, big = 0; long gsum = 0;
        for (int r = 0; r < N; r++) {
            if (par[r] != r) continue;
            comps++; gsum += sum[r];
            if (sz[r] > big) big = sz[r];
            /* walk ring from r: must visit exactly sz[r] members, all with the same label */
            int cnt = 0; long s = 0; int lo = value[r], hi = value[r];
            int x = r;
            do {
                check(find(x) == r, "ring member in component");
                check(label[x] == label[r], "ring member label");
                cnt++; s += value[x];
                if (value[x] < lo) lo = value[x];
                if (value[x] > hi) hi = value[x];
                x = ring[x];
            } while (x != r);
            check(cnt == sz[r], "ring length equals size");
            check(s == sum[r] && lo == mn[r] && hi == mx[r], "aggregates");
        }
        /* label-brute-force component count */
        int lc = 0; for (int i = 0; i < N; i++) if (label[i] == i) lc++;
        check(lc == comps, "component count");
        long total = 0; for (int i = 0; i < N; i++) total += value[i];
        check(total == gsum, "aggregate sum conserved");
        int r0 = find(0);
        printf("round %d: edges=%3d comps=%2d largest=%2d comp(0): size=%2d sum=%5ld min=%4d max=%4d\n",
               round, ne, comps, big, sz[r0], sum[r0], mn[r0], mx[r0]);
    }
    /* enumerate the members of vertex 0's component in ring order, then sorted */
    int mem[N], k = 0, x = 0;
    do { mem[k++] = x; x = ring[x]; } while (x != 0);
    for (int i = 1; i < k; i++) { int v = mem[i], j = i - 1; while (j >= 0 && mem[j] > v) { mem[j + 1] = mem[j]; j--; } mem[j + 1] = v; }
    printf("members of comp(0):");
    for (int i = 0; i < k && i < 20; i++) printf(" %d", mem[i]);
    printf("%s\n", k > 20 ? " ..." : "");
    return 0;
}
