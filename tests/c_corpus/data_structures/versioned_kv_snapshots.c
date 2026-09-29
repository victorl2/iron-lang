/*
 * title: MVCC versioned key-value store with snapshots and garbage collection
 * topic: data_structures
 * covers: multi-version concurrency, version chains, snapshot reads, tombstones, optimistic commit conflicts, GC watermark
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 8080808u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct { long ver; int val; int tomb; } Rev;
typedef struct { Rev *revs; int n, cap; } Chain;

#define NKEYS 40
typedef struct { Chain chain[NKEYS]; long version; long revs_total; } Store;

static void s_init(Store *s) { memset(s, 0, sizeof *s); }
static void s_free(Store *s) { for (int i = 0; i < NKEYS; i++) free(s->chain[i].revs); }
static void s_write(Store *s, long ver, int key, int val, int tomb) {
    Chain *c = &s->chain[key];
    if (c->n && c->revs[c->n - 1].ver == ver) { /* same commit overwrote its own write */
        c->revs[c->n - 1].val = val; c->revs[c->n - 1].tomb = tomb;
        return;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4;
        c->revs = realloc(c->revs, (size_t)c->cap * sizeof(Rev));
        CHECK(c->revs);
    }
    c->revs[c->n].ver = ver; c->revs[c->n].val = val; c->revs[c->n].tomb = tomb;
    c->n++;
    s->revs_total++;
}
/* newest revision visible at snapshot version snap; returns 1 if the key exists there */
static int s_get(const Store *s, long snap, int key, int *out) {
    const Chain *c = &s->chain[key];
    int lo = 0, hi = c->n - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c->revs[mid].ver <= snap) { at = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (at < 0 || c->revs[at].tomb) return 0;
    *out = c->revs[at].val;
    return 1;
}
static long s_last_write(const Store *s, int key) {
    const Chain *c = &s->chain[key];
    return c->n ? c->revs[c->n - 1].ver : 0;
}
static int s_scan_sum(const Store *s, long snap, int lo, int hi, long *sum) {
    int n = 0;
    *sum = 0;
    for (int k = lo; k < hi; k++) { int v; if (s_get(s, snap, k, &v)) { n++; *sum += v; } }
    return n;
}

/* a transaction reads at a snapshot, buffers writes, and commits only if nothing it wrote changed since */
typedef struct { long snap; int wkey[NKEYS], wval[NKEYS], wdel[NKEYS]; int nw; } Txn;
static void t_begin(Txn *t, const Store *s) { t->snap = s->version; t->nw = 0; }
static void t_put(Txn *t, int key, int val, int del) {
    for (int i = 0; i < t->nw; i++) if (t->wkey[i] == key) { t->wval[i] = val; t->wdel[i] = del; return; }
    t->wkey[t->nw] = key; t->wval[t->nw] = val; t->wdel[t->nw] = del; t->nw++;
}
static int t_commit(Txn *t, Store *s) {
    for (int i = 0; i < t->nw; i++)
        if (s_last_write(s, t->wkey[i]) > t->snap) return 0;
    s->version++;
    for (int i = 0; i < t->nw; i++) s_write(s, s->version, t->wkey[i], t->wval[i], t->wdel[i]);
    return 1;
}
/* drop revisions no live snapshot can see: keep the newest revision at or below the watermark and all above it */
static long s_gc(Store *s, long watermark) {
    long dropped = 0;
    for (int k = 0; k < NKEYS; k++) {
        Chain *c = &s->chain[k];
        int keep_from = 0;
        for (int i = 0; i < c->n; i++) if (c->revs[i].ver <= watermark) keep_from = i;
        if (c->n && c->revs[keep_from].ver <= watermark && c->revs[keep_from].tomb) {
            /* a tombstone at the base of the chain hides nothing once it is the oldest visible revision */
            keep_from++;
        }
        if (keep_from > 0) {
            memmove(c->revs, c->revs + keep_from, (size_t)(c->n - keep_from) * sizeof(Rev));
            c->n -= keep_from;
            dropped += keep_from;
        }
    }
    s->revs_total -= dropped;
    return dropped;
}

#define MAXV 400
int main(void) {
    Store s;
    s_init(&s);
    /* model: full state per committed version */
    static int mval[MAXV][NKEYS];
    static unsigned char mex[MAXV][NKEYS];
    long commits = 0, aborts = 0;
    enum { NSNAP = 6 };
    long snaps[NSNAP];
    for (int i = 0; i < NSNAP; i++) snaps[i] = 0;
    long dropped_total = 0;
    for (int step = 0; step < 1500 && s.version < MAXV - 2; step++) {
        Txn a, b;
        t_begin(&a, &s);
        t_begin(&b, &s);
        int na = 1 + (int)(rnd() % 3), nb = 1 + (int)(rnd() % 3);
        for (int i = 0; i < na; i++) { int k = (int)(rnd() % NKEYS); int v = (int)(rnd() % 1000); int del = rnd() % 4 == 0; t_put(&a, k, v, del); }
        for (int i = 0; i < nb; i++) { int k = (int)(rnd() % NKEYS); int v = (int)(rnd() % 1000); int del = rnd() % 4 == 0; t_put(&b, k, v, del); }
        Txn *order[2] = { &a, &b };
        for (int o = 0; o < 2; o++) {
            Txn *t = order[o];
            long before = s.version;
            if (t_commit(t, &s)) {
                CHECK(s.version == before + 1);
                memcpy(mval[s.version], mval[before], sizeof mval[0]);
                memcpy(mex[s.version], mex[before], sizeof mex[0]);
                for (int i = 0; i < t->nw; i++) {
                    mex[s.version][t->wkey[i]] = !t->wdel[i];
                    mval[s.version][t->wkey[i]] = t->wdel[i] ? 0 : t->wval[i];
                }
                commits++;
            } else aborts++;
        }
        if (step % 20 == 0) snaps[rnd() % NSNAP] = s.version;
        if (step % 100 == 99) {
            long wm = snaps[0];
            for (int i = 1; i < NSNAP; i++) if (snaps[i] < wm) wm = snaps[i];
            dropped_total += s_gc(&s, wm);
        }
    }
    /* every snapshot still held, and the newest version, must read exactly like the model */
    long checked = 0;
    for (int i = 0; i <= NSNAP; i++) {
        long v = i < NSNAP ? snaps[i] : s.version;
        for (int k = 0; k < NKEYS; k++) {
            int got = -1;
            int ex = s_get(&s, v, k, &got);
            CHECK(ex == mex[v][k]);
            if (ex) CHECK(got == mval[v][k]);
            checked++;
        }
    }
    long sum = 0;
    int n = s_scan_sum(&s, s.version, 0, NKEYS, &sum);
    long msum = 0;
    int mn = 0;
    for (int k = 0; k < NKEYS; k++) if (mex[s.version][k]) { mn++; msum += mval[s.version][k]; }
    CHECK(n == mn && sum == msum);
    int maxchain = 0;
    for (int k = 0; k < NKEYS; k++) if (s.chain[k].n > maxchain) maxchain = s.chain[k].n;
    printf("version=%ld commits=%ld aborts=%ld\n", s.version, commits, aborts);
    printf("latest state: %d keys, sum %ld; snapshot reads verified=%ld\n", n, sum, checked);
    printf("gc dropped %ld revisions, %ld remain, longest chain %d\n", dropped_total, s.revs_total, maxchain);
    s_free(&s);
    return 0;
}
