/*
 * title: Debug heap with redzones, poisoning, quarantine and leak report
 * topic: memory
 * covers: front and rear redzones, freed-memory poison, delayed reuse quarantine, overflow/underflow/double-free/invalid-free/use-after-free detection, leak reporting, injected bugs matched to detections
 * deps: libc
 */
#define SEED 0xDEB06E4EULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}
static void pat_fill(void *vp, size_t n, unsigned tag) {
    unsigned char *p = vp;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u);
}
static int pat_ok(const void *vp, size_t n, unsigned tag) {
    const unsigned char *p = vp;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u)) return 0;
    return 1;
}

#define HEAP 20480u
#define HDR 24u
#define RZ 16u
#define MAGIC 0xB10CB10Cu
#define QCAP 8
enum { ST_FREE, ST_ALLOC, ST_QUAR };
enum { E_OK, E_OVERFLOW, E_UNDERFLOW, E_DOUBLE, E_INVALID, E_UAF, NERR };
static const char *err_name[NERR] = { "ok", "overflow", "underflow", "double-free", "invalid-free", "use-after-free" };

static _Alignas(16) unsigned char heap[HEAP];
static unsigned quarantine[QCAP], qn, qh;
static unsigned long detected[NERR], injected[NERR];
static unsigned seq_counter;
static unsigned long allocs, frees;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned total(unsigned p) { return rd(p + 4); }
static unsigned state(unsigned p) { return rd(p + 8); }
static unsigned req(unsigned p) { return rd(p + 12); }
static unsigned rounded(unsigned r) { return (r + 7u) & ~7u; }
static unsigned payload_of(unsigned p) { return p + HDR + RZ; }

static void write_block(unsigned p, unsigned tot, unsigned st, unsigned r, unsigned seq) {
    wr(p, MAGIC); wr(p + 4, tot); wr(p + 8, st); wr(p + 12, r); wr(p + 16, seq); wr(p + 20, 0);
}

static void heap_init(void) { write_block(0, HEAP, ST_FREE, 0, 0); }

static long d_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = HDR + RZ + rounded((unsigned)n) + RZ;
    for (unsigned p = 0; p < HEAP; p += total(p)) {
        CHECK(rd(p) == MAGIC);
        if (state(p) != ST_FREE) continue;
        while (p + total(p) < HEAP && state(p + total(p)) == ST_FREE) write_block(p, total(p) + total(p + total(p)), ST_FREE, 0, 0);
        unsigned sz = total(p);
        if (sz < need) continue;
        if (sz - need >= HDR + 2 * RZ + 8) { write_block(p + need, sz - need, ST_FREE, 0, 0); sz = need; }
        write_block(p, sz, ST_ALLOC, (unsigned)n, ++seq_counter);
        memset(heap + p + HDR, 0xFD, RZ);
        memset(heap + payload_of(p) + rounded((unsigned)n), 0xFD, sz - HDR - RZ - rounded((unsigned)n));
        allocs++;
        return (long)payload_of(p);
    }
    return -1;
}

static int rz_ok(unsigned from, unsigned len) {
    for (unsigned i = 0; i < len; i++) if (heap[from + i] != 0xFD) return 0;
    return 1;
}

static void release_oldest(void) {
    unsigned p = quarantine[qh];
    qh = (qh + 1) % QCAP; qn--;
    unsigned pay = payload_of(p), len = total(p) - HDR - RZ - RZ;
    for (unsigned i = 0; i < len; i++) if (heap[pay + i] != 0xDD) { detected[E_UAF]++; break; }
    write_block(p, total(p), ST_FREE, 0, 0);
}

static int d_free(unsigned payload) {
    /* locate the block that owns exactly this payload address */
    unsigned found = HEAP;
    for (unsigned p = 0; p < HEAP; p += total(p)) {
        CHECK(rd(p) == MAGIC);
        if (payload_of(p) == payload) { found = p; break; }
    }
    if (found == HEAP) { detected[E_INVALID]++; return E_INVALID; }
    unsigned p = found;
    if (state(p) != ST_ALLOC) { detected[E_DOUBLE]++; return E_DOUBLE; }
    unsigned r = rounded(req(p));
    if (!rz_ok(p + HDR, RZ)) { detected[E_UNDERFLOW]++; return E_UNDERFLOW; }
    if (!rz_ok(payload + r, total(p) - HDR - RZ - r)) { detected[E_OVERFLOW]++; return E_OVERFLOW; }
    memset(heap + payload, 0xDD, total(p) - HDR - RZ - RZ);
    wr(p + 8, ST_QUAR);
    if (qn == QCAP) release_oldest();
    quarantine[(qh + qn) % QCAP] = p; qn++;
    frees++;
    return E_OK;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

static Rec live[500];
static int nlive;
static unsigned tagc = 1;

static void churn(int steps) {
    for (int s = 0; s < steps; s++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 500)) {
            size_t n = 1 + rnd() % 120;
            long off = d_alloc(n);
            if (off < 0) continue;
            pat_fill(heap + off, n, tagc);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tagc++; nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            CHECK(d_free(live[i].off) == E_OK);
            live[i] = live[--nlive];
        }
    }
}

int main(void) {
    heap_init();
    churn(6000);
    for (int e = 1; e < NERR; e++) CHECK(detected[e] == 0);          /* no false positives on valid traffic */
    printf("phase 1: %lu allocs, %lu frees, zero false positives, %d live\n", allocs, frees, nlive);

    for (int round = 0; round < 60; round++) {
        churn(30);
        unsigned kind = (unsigned)round % 5;
        long off = d_alloc(1 + rnd() % 90);
        if (off < 0) continue;
        unsigned p = (unsigned)off - HDR - RZ;
        unsigned r = rounded(req(p));
        if (kind == 0) {
            injected[E_OVERFLOW]++;
            heap[off + r] = 0x00;                       /* first byte past the payload */
            CHECK(d_free((unsigned)off) == E_OVERFLOW);
            heap[off + r] = 0xFD;
            CHECK(d_free((unsigned)off) == E_OK);
        } else if (kind == 1) {
            injected[E_UNDERFLOW]++;
            heap[off - 1] = 0x42;
            CHECK(d_free((unsigned)off) == E_UNDERFLOW);
            heap[off - 1] = 0xFD;
            CHECK(d_free((unsigned)off) == E_OK);
        } else if (kind == 2) {
            injected[E_DOUBLE]++;
            CHECK(d_free((unsigned)off) == E_OK);
            CHECK(d_free((unsigned)off) == E_DOUBLE);
        } else if (kind == 3) {
            injected[E_INVALID]++;
            CHECK(d_free((unsigned)off + 1) == E_INVALID);
            injected[E_INVALID]++;
            CHECK(d_free(HEAP + 40) == E_INVALID);
            CHECK(d_free((unsigned)off) == E_OK);
        } else {
            injected[E_UAF]++;
            CHECK(d_free((unsigned)off) == E_OK);
            heap[off + 1] = 0x11;                       /* write through a dangling pointer */
            unsigned long before = detected[E_UAF];
            /* push QCAP more blocks through quarantine so the corrupted one is examined */
            for (int k = 0; k < QCAP + 1; k++) {
                long o = d_alloc(8);
                CHECK(o >= 0);
                CHECK(d_free((unsigned)o) == E_OK);
            }
            CHECK(detected[E_UAF] == before + 1);
        }
    }
    printf("phase 2: injected bugs vs detections\n");
    for (int e = 1; e < NERR; e++) {
        printf("  %-15s injected=%2lu detected=%2lu\n", err_name[e], injected[e], detected[e]);
        CHECK(injected[e] == detected[e]);
    }

    /* leaks: whatever is still live at exit is reported with allocation sequence numbers */
    unsigned leaks = 0, leak_bytes = 0, min_seq = ~0u, max_seq = 0;
    for (unsigned p = 0; p < HEAP; p += total(p)) {
        if (state(p) == ST_ALLOC) {
            leaks++; leak_bytes += req(p);
            unsigned s = rd(p + 16);
            if (s < min_seq) min_seq = s;
            if (s > max_seq) max_seq = s;
        }
    }
    size_t expect = 0;
    for (int i = 0; i < nlive; i++) expect += live[i].n;
    CHECK(leaks == (unsigned)nlive && leak_bytes == expect);
    printf("leak report: %u blocks, %u bytes, alloc sequence %u..%u\n", leaks, leak_bytes, min_seq, max_seq);
    for (int i = 0; i < nlive; i++) CHECK(d_free(live[i].off) == E_OK);
    while (qn) release_oldest();
    CHECK(detected[E_UAF] == injected[E_UAF]);
    printf("cleaned up; quarantine empty, total allocations=%u\n", seq_counter);
    return 0;
}
