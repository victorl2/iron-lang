/*
 * title: Konig minimum vertex cover from a maximum matching
 * topic: algorithms
 * covers: bipartite matching, Konig's theorem, alternating paths, vertex cover, independent set, subset brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { L = 10, R = 10 };

static unsigned st = 1618033u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

static unsigned adj[L]; /* bitmask over right vertices */
static int ml[L], mr[R], vis[R];

static int try_kuhn(int u) {
    for (int v = 0; v < R; v++) {
        if (!(adj[u] >> v & 1u) || vis[v]) continue;
        vis[v] = 1;
        if (mr[v] < 0 || try_kuhn(mr[v])) {
            ml[u] = v;
            mr[v] = u;
            return 1;
        }
    }
    return 0;
}

static int bits(unsigned x) {
    int c = 0;
    for (; x; x &= x - 1) c++;
    return c;
}

int main(void) {
    for (int trial = 0; trial < 8; trial++) {
        unsigned density = 7 + 2u * (unsigned)trial;
        int edges = 0;
        for (int u = 0; u < L; u++) {
            adj[u] = 0;
            for (int v = 0; v < R; v++)
                if (rnd() % 100 < density) adj[u] |= 1u << v, edges++;
        }
        memset(ml, -1, sizeof ml);
        memset(mr, -1, sizeof mr);
        int match = 0;
        for (int u = 0; u < L; u++) {
            memset(vis, 0, sizeof vis);
            match += try_kuhn(u);
        }
        /* Z = vertices reachable from unmatched left via alternating paths */
        int zl[L] = {0}, zr[R] = {0}, stack[L + R], sp = 0;
        for (int u = 0; u < L; u++)
            if (ml[u] < 0) zl[u] = 1, stack[sp++] = u;
        while (sp) {
            int u = stack[--sp];
            for (int v = 0; v < R; v++)
                if ((adj[u] >> v & 1u) && !zr[v] && ml[u] != v) {
                    zr[v] = 1;
                    if (mr[v] >= 0 && !zl[mr[v]]) zl[mr[v]] = 1, stack[sp++] = mr[v];
                }
        }
        /* cover = (L \ Z) + (R ∩ Z) */
        unsigned coverL = 0, coverR = 0;
        for (int u = 0; u < L; u++)
            if (!zl[u]) coverL |= 1u << u;
        for (int v = 0; v < R; v++)
            if (zr[v]) coverR |= 1u << v;
        int csize = bits(coverL) + bits(coverR);
        check(csize == match, "konig: cover size equals matching size");
        for (int u = 0; u < L; u++)
            for (int v = 0; v < R; v++)
                if (adj[u] >> v & 1u) check((coverL >> u & 1u) || (coverR >> v & 1u), "every edge covered");
        /* brute force: smallest cover by choosing the left subset and forcing the right side */
        int best = L + R;
        for (unsigned sl = 0; sl < (1u << L); sl++) {
            unsigned need = 0;
            for (int u = 0; u < L; u++)
                if (!(sl >> u & 1u)) need |= adj[u];
            int c = bits(sl) + bits(need);
            if (c < best) best = c;
        }
        check(best == match, "brute-force cover equals matching");
        printf("trial %d: edges %2d matching %2d cover L=%2d R=%2d independent set %2d\n", trial, edges, match, bits(coverL), bits(coverR),
               L + R - csize);
    }
    return 0;
}
