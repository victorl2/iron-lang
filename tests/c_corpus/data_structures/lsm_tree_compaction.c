/*
 * title: LSM tree with memtable, sorted runs and compaction
 * topic: data_structures
 * covers: lsm tree, memtable flush, sorted runs, tiered compaction, tombstones, bloom filters, range scan merge
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MEMCAP 32
#define FANOUT 3
#define MAXLEVEL 8
#define KEYS 1000

typedef struct { int key, val, tomb; } Rec;
typedef struct {
    Rec *r;
    int n;
    uint64_t bloom[8]; /* 512 bits */
} Run;
typedef struct { Run *run[FANOUT + 1]; int nrun; } Level;

static Rec mem[MEMCAP + 1];
static int nmem;
static Level lv[MAXLEVEL];
static long flushes, compactions, rec_written, bloom_skips, run_probes, tomb_dropped;

static uint64_t rs = 0x15AB1E5ULL * 7;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static unsigned h1(int k) { uint32_t x = (uint32_t)k * 2654435761u; x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; return x; }
static void bloom_add(Run *r, int k) {
    uint32_t h = h1(k), g = h1(k + 977);
    for (int i = 0; i < 3; i++) { unsigned b = (h + (unsigned)i * g) & 511; r->bloom[b >> 6] |= 1ULL << (b & 63); }
}
static int bloom_may(const Run *r, int k) {
    uint32_t h = h1(k), g = h1(k + 977);
    for (int i = 0; i < 3; i++) { unsigned b = (h + (unsigned)i * g) & 511; if (!(r->bloom[b >> 6] & (1ULL << (b & 63)))) return 0; }
    return 1;
}
static Run *make_run(Rec *recs, int n) {
    Run *r = calloc(1, sizeof *r);
    r->r = malloc(sizeof(Rec) * (size_t)(n ? n : 1));
    memcpy(r->r, recs, sizeof(Rec) * (size_t)n);
    r->n = n;
    for (int i = 0; i < n; i++) bloom_add(r, recs[i].key);
    rec_written += n;
    return r;
}
static void free_run(Run *r) { free(r->r); free(r); }

static int deepest_nonempty(void) { int d = -1; for (int i = 0; i < MAXLEVEL; i++) if (lv[i].nrun) d = i; return d; }

/* merge runs (oldest first) into one; newest value wins; drop tombstones at bottom */
static Run *merge_runs(Run **runs, int nr, int drop_tombs) {
    int total = 0;
    for (int i = 0; i < nr; i++) total += runs[i]->n;
    Rec *out = malloc(sizeof(Rec) * (size_t)(total + 1));
    int pos[FANOUT + 1] = {0}, n = 0;
    for (;;) {
        int mk = 1 << 30;
        for (int i = 0; i < nr; i++) if (pos[i] < runs[i]->n && runs[i]->r[pos[i]].key < mk) mk = runs[i]->r[pos[i]].key;
        if (mk == 1 << 30) break;
        Rec win; int have = 0;
        for (int i = 0; i < nr; i++) if (pos[i] < runs[i]->n && runs[i]->r[pos[i]].key == mk) { win = runs[i]->r[pos[i]++]; have = 1; } /* later runs are newer */
        check(have, "some run has the min key");
        if (win.tomb && drop_tombs) { tomb_dropped++; continue; }
        out[n++] = win;
    }
    Run *r = make_run(out, n);
    free(out);
    return r;
}
static void add_run(int level, Run *r) {
    check(level < MAXLEVEL, "level bound");
    lv[level].run[lv[level].nrun++] = r;
    if (lv[level].nrun == FANOUT) {
        compactions++;
        int deepest = deepest_nonempty();
        int bottom = deepest <= level; /* nothing below this level */
        Run *m = merge_runs(lv[level].run, FANOUT, bottom);
        for (int i = 0; i < FANOUT; i++) free_run(lv[level].run[i]);
        lv[level].nrun = 0;
        add_run(level + 1, m);
    }
}
static int cmp_rec(const void *a, const void *b) { return ((const Rec *)a)->key - ((const Rec *)b)->key; }
static void flush(void) {
    if (!nmem) return;
    flushes++;
    qsort(mem, (size_t)nmem, sizeof(Rec), cmp_rec);
    Run *r = make_run(mem, nmem);
    nmem = 0;
    add_run(0, r);
}
static void put_rec(int k, int v, int tomb) {
    for (int i = 0; i < nmem; i++) if (mem[i].key == k) { mem[i].val = v; mem[i].tomb = tomb; return; }
    mem[nmem].key = k; mem[nmem].val = v; mem[nmem].tomb = tomb; nmem++;
    if (nmem == MEMCAP) flush();
}
static void put(int k, int v) { put_rec(k, v, 0); }
static void del(int k) { put_rec(k, 0, 1); }
static int run_find(const Run *r, int k) {
    int lo = 0, hi = r->n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (r->r[m].key == k) return m; if (r->r[m].key < k) lo = m + 1; else hi = m - 1; }
    return -1;
}
/* returns 1 and sets *v if present */
static int get(int k, int *v) {
    for (int i = 0; i < nmem; i++) if (mem[i].key == k) { if (mem[i].tomb) return 0; *v = mem[i].val; return 1; }
    for (int l = 0; l < MAXLEVEL; l++)
        for (int i = lv[l].nrun - 1; i >= 0; i--) {
            const Run *r = lv[l].run[i];
            if (!bloom_may(r, k)) { bloom_skips++; continue; }
            run_probes++;
            int p = run_find(r, k);
            if (p >= 0) { if (r->r[p].tomb) return 0; *v = r->r[p].val; return 1; }
        }
    return 0;
}
/* scan [lo,hi]: gather newest version per key, oldest sources first so newer overwrite */
static int scan(int lo, int hi, int *keys, int *vals) {
    static int have[KEYS], val[KEYS], tomb[KEYS];
    memset(have, 0, sizeof have);
    for (int l = MAXLEVEL - 1; l >= 0; l--)
        for (int i = 0; i < lv[l].nrun; i++) {
            const Run *r = lv[l].run[i];
            for (int j = 0; j < r->n; j++) if (r->r[j].key >= lo && r->r[j].key <= hi) { have[r->r[j].key] = 1; val[r->r[j].key] = r->r[j].val; tomb[r->r[j].key] = r->r[j].tomb; }
        }
    for (int i = 0; i < nmem; i++) if (mem[i].key >= lo && mem[i].key <= hi) { have[mem[i].key] = 1; val[mem[i].key] = mem[i].val; tomb[mem[i].key] = mem[i].tomb; }
    int n = 0;
    for (int k = lo; k <= hi; k++) if (have[k] && !tomb[k]) { keys[n] = k; vals[n] = val[k]; n++; }
    return n;
}
static int total_recs(void) { int t = 0; for (int l = 0; l < MAXLEVEL; l++) for (int i = 0; i < lv[l].nrun; i++) t += lv[l].run[i]->n; return t; }
static void shape(char *o) {
    o[0] = 0;
    for (int l = 0; l < MAXLEVEL; l++) if (lv[l].nrun) {
        char t[64]; int sum = 0; for (int i = 0; i < lv[l].nrun; i++) sum += lv[l].run[i]->n;
        snprintf(t, sizeof t, "L%d:%druns/%drecs ", l, lv[l].nrun, sum); strcat(o, t);
    }
}

int main(void) {
    static int refv[KEYS], refp[KEYS];
    long puts_ = 0, dels = 0, gets = 0, found = 0;
    for (int step = 0; step < 12000; step++) {
        int k = (int)(rnd() % KEYS);
        if (step % 4000 < 200) k = k % 50; /* hot spot phases create many overwrites */
        unsigned op = rnd() % 10;
        if (op < 6) { int v = (int)(rnd() % 100000); put(k, v); refv[k] = v; refp[k] = 1; puts_++; }
        else if (op < 8) { del(k); refp[k] = 0; dels++; }
        else {
            int v = -1; int f = get(k, &v);
            check(f == refp[k], "get presence");
            if (f) { check(v == refv[k], "get value"); found++; }
            gets++;
        }
        if (step % 1500 == 1499) {
            int keys[KEYS], vals[KEYS];
            int lo = (int)(rnd() % 500), hi = lo + 300;
            int n = scan(lo, hi, keys, vals);
            int want = 0;
            for (int q = lo; q <= hi; q++) if (refp[q]) { check(want < n && keys[want] == q && vals[want] == refv[q], "scan entries"); want++; }
            check(n == want, "scan length");
            char sh[256]; shape(sh);
            printf("step %5d: mem=%2d scan[%d,%d]=%3d keys, %s\n", step + 1, nmem, lo, hi, n, sh);
        }
    }
    /* full verification of every key */
    int live = 0;
    for (int k = 0; k < KEYS; k++) {
        int v = -1, f = get(k, &v);
        check(f == refp[k] && (!f || v == refv[k]), "final content");
        live += f;
    }
    printf("puts %ld deletes %ld gets %ld (found %ld)\n", puts_, dels, gets, found);
    printf("flushes %ld compactions %ld records written %ld tombstones dropped %ld\n", flushes, compactions, rec_written, tomb_dropped);
    printf("bloom skipped %ld run probes, %ld run lookups done; live keys %d; stored records %d\n", bloom_skips, run_probes, live, total_recs());
    printf("write amplification %ld.%02ld\n", rec_written / (puts_ + dels), (rec_written * 100 / (puts_ + dels)) % 100);
    for (int l = 0; l < MAXLEVEL; l++) for (int i = 0; i < lv[l].nrun; i++) {
        const Run *r = lv[l].run[i];
        for (int j = 1; j < r->n; j++) check(r->r[j - 1].key < r->r[j].key, "runs strictly sorted");
        free_run(lv[l].run[i]);
    }
    return 0;
}
