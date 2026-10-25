/*
 * title: First, best, worst and next fit compared on one trace
 * topic: memory
 * covers: placement policies, sorted free extent table, identical replayed trace, external fragmentation metrics, failure counts, fill-pattern validation
 * deps: libc
 */
#define SEED 0xF17C0DEULL
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

#define ARENA 16384u
#define MAXEXT 1024
#define MAXOPS 9000
#define MAXLIVE 150

typedef struct { unsigned start, len; } Ext;
enum { FIRST, BEST, WORST, NEXT, NPOL };
static const char *pol_name[NPOL] = { "first", "best", "worst", "next" };

static _Alignas(16) unsigned char mem[ARENA];
static Ext fx[MAXEXT];
static int nfx;
static unsigned next_pos;    /* address where next-fit resumes */

typedef struct { int is_alloc; unsigned size; int target; } Op;
static Op ops[MAXOPS];
static int nops;

static void ext_insert(unsigned start, unsigned len) {
    int i = 0;
    while (i < nfx && fx[i].start < start) i++;
    CHECK(nfx < MAXEXT);
    memmove(&fx[i + 1], &fx[i], (size_t)(nfx - i) * sizeof(Ext));
    fx[i].start = start; fx[i].len = len; nfx++;
    if (i + 1 < nfx && fx[i].start + fx[i].len == fx[i + 1].start) {
        fx[i].len += fx[i + 1].len;
        memmove(&fx[i + 1], &fx[i + 2], (size_t)(nfx - i - 2) * sizeof(Ext));
        nfx--;
    }
    if (i > 0 && fx[i - 1].start + fx[i - 1].len == fx[i].start) {
        fx[i - 1].len += fx[i].len;
        memmove(&fx[i], &fx[i + 1], (size_t)(nfx - i - 1) * sizeof(Ext));
        nfx--;
    }
}

static int find(int pol, unsigned len, unsigned long *steps) {
    int best = -1;
    int start_i = 0;
    if (pol == NEXT) { while (start_i < nfx && fx[start_i].start < next_pos) start_i++; }
    for (int k = 0; k < nfx; k++) {
        int i = pol == NEXT ? (start_i + k) % nfx : k;
        (*steps)++;
        if (fx[i].len < len) continue;
        if (pol == FIRST || pol == NEXT) return i;
        if (best < 0 || (pol == BEST && fx[i].len < fx[best].len) || (pol == WORST && fx[i].len > fx[best].len)) best = i;
    }
    return best;
}

typedef struct { int ok; unsigned start, len; unsigned tag; } Slot;

static void gen_trace(void) {
    int live[MAXLIVE], nl = 0;
    for (int i = 0; i < MAXOPS; i++) {
        if (nl == 0 || (rnd() % 100 < 52 && nl < MAXLIVE)) {
            unsigned k = rnd() % 10;
            unsigned sz = k < 6 ? 8 * (1 + rnd() % 8) : k < 9 ? 8 * (5 + rnd() % 15) : 8 * (30 + rnd() % 30);
            ops[nops].is_alloc = 1; ops[nops].size = sz; ops[nops].target = -1;
            live[nl++] = nops++;
        } else {
            int j = (int)(rnd() % (unsigned)nl);
            ops[nops].is_alloc = 0; ops[nops].size = 0; ops[nops].target = live[j];
            live[j] = live[--nl]; nops++;
        }
    }
}

static void run(int pol) {
    static Slot slot[MAXOPS];
    memset(slot, 0, sizeof slot);
    nfx = 0; next_pos = 0;
    ext_insert(0, ARENA);
    unsigned long steps = 0, fails = 0, extent_sum = 0, samples = 0;
    unsigned max_ext = 0, live_bytes = 0, peak_live = 0, worst_frag = 0;
    unsigned req_fail_bytes = 0;
    for (int i = 0; i < nops; i++) {
        if (ops[i].is_alloc) {
            unsigned len = ops[i].size;
            int e = find(pol, len, &steps);
            if (e < 0) { fails++; req_fail_bytes += len; continue; }
            unsigned st = fx[e].start;
            next_pos = st + len;
            if (fx[e].len == len) { memmove(&fx[e], &fx[e + 1], (size_t)(nfx - e - 1) * sizeof(Ext)); nfx--; }
            else { fx[e].start += len; fx[e].len -= len; }
            slot[i].ok = 1; slot[i].start = st; slot[i].len = len; slot[i].tag = (unsigned)i + 1;
            pat_fill(mem + st, len, slot[i].tag);
            live_bytes += len;
            if (live_bytes > peak_live) peak_live = live_bytes;
        } else {
            Slot *s = &slot[ops[i].target];
            if (!s->ok) continue;
            CHECK(pat_ok(mem + s->start, s->len, s->tag));
            ext_insert(s->start, s->len);
            live_bytes -= s->len;
            s->ok = 0;
        }
        extent_sum += (unsigned long)nfx; samples++;
        if ((unsigned)nfx > max_ext) max_ext = (unsigned)nfx;
        if (i % 50 == 0) {
            unsigned total_free = 0, largest = 0;
            for (int k = 0; k < nfx; k++) {
                total_free += fx[k].len; if (fx[k].len > largest) largest = fx[k].len;
                CHECK(k == 0 || fx[k - 1].start + fx[k - 1].len < fx[k].start);
            }
            CHECK(total_free + live_bytes == ARENA);
            if (total_free) { unsigned f = 1000 - 1000 * largest / total_free; if (f > worst_frag && total_free > 512) worst_frag = f; }
            for (int j = 0; j <= i; j++) if (slot[j].ok) CHECK(pat_ok(mem + slot[j].start, slot[j].len, slot[j].tag));
        }
    }
    unsigned total_free = 0, largest = 0;
    for (int k = 0; k < nfx; k++) { total_free += fx[k].len; if (fx[k].len > largest) largest = fx[k].len; }
    printf("%-5s fails=%4lu (%5u B) steps=%7lu avg extents=%5.2f max extents=%3u\n", pol_name[pol], fails, req_fail_bytes, steps, (double)extent_sum / (double)samples, max_ext);
    printf("      final: free=%5u in %3d extents, largest=%5u, peak live=%5u, worst sampled frag=%u/1000\n", total_free, nfx, largest, peak_live, worst_frag);
}

int main(void) {
    gen_trace();
    int allocs = 0;
    for (int i = 0; i < nops; i++) allocs += ops[i].is_alloc;
    printf("trace: %d ops, %d allocations, %d frees\n", nops, allocs, nops - allocs);
    for (int p = 0; p < NPOL; p++) run(p);
    return 0;
}
