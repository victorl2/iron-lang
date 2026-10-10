/*
 * title: Quotient filter with run and cluster metadata bits
 * topic: data_structures
 * covers: quotient filter, is_occupied/is_continuation/is_shifted bits, run start search, cluster decode, delete by cluster rebuild
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
enum { QB = 10, RB = 8, NSLOTS = 1 << QB };
typedef struct {
    uint8_t rem[NSLOTS];
    uint8_t occ[NSLOTS];  /* bucket i has a run somewhere */
    uint8_t cont[NSLOTS]; /* slot holds a non-head element of a run */
    uint8_t shf[NSLOTS];  /* slot holds an element that is not in its home bucket */
    int n;
    long shifts_done;
} QF;

static int nx(int i) { return (i + 1) & (NSLOTS - 1); }
static int pv(int i) { return (i - 1) & (NSLOTS - 1); }
static int slot_empty(const QF *q, int i) { return !q->occ[i] && !q->cont[i] && !q->shf[i]; }

/* index of the first slot of the run belonging to bucket fq (fq must be occupied) */
static int run_start(const QF *q, int fq) {
    int b = fq;
    while (q->shf[b])
        b = pv(b);
    int s = b;
    while (b != fq) {
        do
            s = nx(s);
        while (q->cont[s]);
        do
            b = nx(b);
        while (!q->occ[b]);
    }
    return s;
}

static void split_fp(uint64_t key, int *fq, uint8_t *fr) {
    uint64_t f = mix64(key) & ((1u << (QB + RB)) - 1);
    *fq = (int)(f >> RB);
    *fr = (uint8_t)(f & 0xff);
}

static int qf_has_fp(const QF *q, int fq, uint8_t fr) {
    if (!q->occ[fq])
        return 0;
    int s = run_start(q, fq);
    do {
        if (q->rem[s] == fr)
            return 1;
        if (q->rem[s] > fr)
            return 0;
        s = nx(s);
    } while (q->cont[s]);
    return 0;
}

/* insert; returns 1 if added, 0 if the fingerprint was already present */
static int qf_add_fp(QF *q, int fq, uint8_t fr) {
    check(q->n < NSLOTS * 9 / 10, "load limit");
    if (slot_empty(q, fq)) {
        q->rem[fq] = fr;
        q->occ[fq] = 1;
        q->n++;
        return 1;
    }
    int had_run = q->occ[fq];
    if (had_run && qf_has_fp(q, fq, fr))
        return 0;
    q->occ[fq] = 1;
    int s = run_start(q, fq), p = s;
    if (had_run) {
        do {
            if (q->rem[p] > fr)
                break;
            p = nx(p);
        } while (q->cont[p]);
    }
    /* make room at p by shifting the rest of the cluster one slot right */
    int e = p;
    while (!slot_empty(q, e))
        e = nx(e);
    for (int i = e; i != p; i = pv(i)) {
        int j = pv(i);
        q->rem[i] = q->rem[j];
        q->cont[i] = q->cont[j];
        q->shf[i] = 1;
        q->shifts_done++;
    }
    int becomes_head = !had_run || p == s;
    if (had_run && p == s)
        q->cont[nx(p)] = 1; /* old head becomes a continuation */
    q->rem[p] = fr;
    q->cont[p] = !becomes_head;
    q->shf[p] = p != fq;
    q->n++;
    return 1;
}

typedef struct {
    int q;
    uint8_t r;
} Item;

/* decode the cluster that begins at slot c; returns number of items, sets *len to slots covered */
static int decode_cluster(const QF *qf, int c, Item *out, int *len) {
    int cnt = 0, q = c, i = c;
    int first = 1;
    while (1) {
        if (slot_empty(qf, i))
            break;
        if (!qf->cont[i]) {
            if (!first && !qf->shf[i])
                break; /* next cluster starts here */
            if (!first) {
                do
                    q = nx(q);
                while (!qf->occ[q]);
            }
            first = 0;
        }
        out[cnt].q = q;
        out[cnt].r = qf->rem[i];
        cnt++;
        i = nx(i);
        if (i == c)
            break;
    }
    *len = cnt;
    return cnt;
}

static int qf_del_fp(QF *q, int fq, uint8_t fr) {
    if (!qf_has_fp(q, fq, fr))
        return 0;
    int c = run_start(q, fq);
    while (q->shf[c])
        c = pv(c); /* walk back to the cluster start */
    Item items[NSLOTS];
    int len;
    decode_cluster(q, c, items, &len);
    for (int i = 0, s = c; i < len; i++, s = nx(s)) {
        q->occ[items[i].q] = 0;
        q->rem[s] = 0;
        q->cont[s] = 0;
        q->shf[s] = 0;
    }
    q->n -= len;
    int removed = 0;
    for (int i = 0; i < len; i++) {
        if (!removed && items[i].q == fq && items[i].r == fr) {
            removed = 1;
            continue;
        }
        qf_add_fp(q, items[i].q, items[i].r);
    }
    check(removed, "found in cluster");
    return 1;
}

/* fingerprints stored in the reference as (q<<8|r) */
static int ref_fp_find(const int *a, int n, int v) {
    for (int i = 0; i < n; i++)
        if (a[i] == v)
            return i;
    return -1;
}

int main(void) {
    static QF q;
    static int refset[NSLOTS];
    int rn = 0;
    long adds = 0, dups = 0, dels = 0, hits = 0;
    for (int step = 0; step < 40000; step++) {
        uint64_t key = rnd() % 5000;
        int fq;
        uint8_t fr;
        split_fp(key, &fq, &fr);
        int fpv = (fq << 8) | fr;
        int ri = ref_fp_find(refset, rn, fpv);
        unsigned op = (unsigned)(rnd() % 10);
        if (op < 4 && q.n < NSLOTS * 8 / 10) {
            int added = qf_add_fp(&q, fq, fr);
            check(added == (ri < 0), "add result");
            if (added)
                refset[rn++] = fpv;
            adds += added;
            dups += !added;
        } else if (op < 7) {
            int d = qf_del_fp(&q, fq, fr);
            check(d == (ri >= 0), "del result");
            if (d) {
                refset[ri] = refset[--rn];
                dels++;
            }
        } else {
            int h = qf_has_fp(&q, fq, fr);
            check(h == (ri >= 0), "membership equals fingerprint set");
            hits += h;
        }
        check(q.n == rn, "count");
    }
    /* full decode of every cluster must equal the reference set exactly */
    static Item items[NSLOTS];
    int seen = 0;
    long clusters = 0, maxcluster = 0, runs = 0;
    for (int i = 0; i < NSLOTS; i++) {
        if (!slot_empty(&q, i) && !q.shf[i] && !q.cont[i]) {
            int len;
            decode_cluster(&q, i, items, &len);
            clusters++;
            if (len > maxcluster)
                maxcluster = len;
            for (int j = 0; j < len; j++) {
                int v = (items[j].q << 8) | items[j].r;
                check(ref_fp_find(refset, rn, v) >= 0, "decoded item in reference");
                seen++;
            }
        }
        runs += q.occ[i];
    }
    check(seen == rn, "decoded count equals reference");
    /* runs are sorted by remainder */
    for (int i = 0; i < NSLOTS; i++)
        if (q.cont[i])
            check(q.rem[pv(i)] <= q.rem[i], "sorted runs");
    printf("adds=%ld duplicate fingerprints=%ld deletes=%ld positive lookups=%ld\n", adds, dups, dels, hits);
    printf("stored=%d slots=%d load=%.4f runs=%ld clusters=%ld longest cluster=%ld shifts=%ld\n", q.n, NSLOTS, (double)q.n / NSLOTS, runs,
           clusters, maxcluster, q.shifts_done);
    /* measured false positive rate against keys that were never inserted (distinct fingerprints only) */
    long fp = 0, trials = 0;
    for (uint64_t k = 100000; k < 120000; k++) {
        int fq;
        uint8_t fr;
        split_fp(k, &fq, &fr);
        trials++;
        fp += qf_has_fp(&q, fq, fr);
    }
    printf("false positive rate for unseen keys=%.4f\n", (double)fp / (double)trials);
    return 0;
}
