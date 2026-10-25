/*
 * title: Worst-fit allocator with out-of-band tags and a max-heap of free blocks
 * topic: memory
 * covers: worst fit, out-of-band block tables, binary max-heap with position index, coalescing via tables, remainder reinsertion
 * deps: libc
 */
#define SEED 0x0F2F17ULL
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

#define UNIT 8u
#define NU 1024u                 /* 8 KiB heap */

static _Alignas(16) unsigned char heap[NU * UNIT];
/* out-of-band metadata, indexed by unit */
static uint16_t head_sz[NU];     /* block size in units at the first unit of a block, else 0 */
static uint16_t tail_sz[NU];     /* block size in units at the last unit of a block, else 0 */
static unsigned char is_free[NU];
static int32_t hpos[NU];         /* position in max-heap, -1 if not in heap */
static uint16_t mh[NU];          /* heap of block start units */
static int hn;
static unsigned long allocs, frees, splits, merges, heap_moves;

static int better(unsigned a, unsigned b) {  /* a should be above b: larger first, lower address on ties */
    if (head_sz[a] != head_sz[b]) return head_sz[a] > head_sz[b];
    return a < b;
}
static void hset(int i, unsigned b) { mh[i] = (uint16_t)b; hpos[b] = i; heap_moves++; }
static void sift_up(int i) {
    unsigned b = mh[i];
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!better(b, mh[p])) break;
        hset(i, mh[p]); i = p;
    }
    hset(i, b);
}
static void sift_down(int i) {
    unsigned b = mh[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= hn) break;
        if (c + 1 < hn && better(mh[c + 1], mh[c])) c++;
        if (!better(mh[c], b)) break;
        hset(i, mh[c]); i = c;
    }
    hset(i, b);
}
static void heap_push(unsigned b) { mh[hn] = (uint16_t)b; hn++; sift_up(hn - 1); }
static void heap_remove(unsigned b) {
    int i = hpos[b];
    CHECK(i >= 0 && mh[i] == b);
    hpos[b] = -1;
    hn--;
    if (i == hn) return;
    mh[i] = mh[hn];
    hpos[mh[i]] = i;
    sift_up(i);
    sift_down(hpos[mh[i]] );
}

static void set_block(unsigned s, unsigned len, int fr) {
    head_sz[s] = (uint16_t)len; tail_sz[s + len - 1] = (uint16_t)len;
    is_free[s] = (unsigned char)fr;
}

static void heap_init(void) {
    for (unsigned i = 0; i < NU; i++) hpos[i] = -1;
    set_block(0, NU, 1);
    heap_push(0);
}

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned units = (unsigned)((n + UNIT - 1) / UNIT);
    if (hn == 0 || head_sz[mh[0]] < units) return -1;
    unsigned s = mh[0], len = head_sz[s];
    heap_remove(s);
    if (len > units) {
        head_sz[s + units] = 0;
        set_block(s + units, len - units, 1);
        heap_push(s + units);
        tail_sz[s + units - 1] = 0;
        splits++;
    }
    tail_sz[s + len - 1] = (uint16_t)(len > units ? tail_sz[s + len - 1] : len);
    set_block(s, units, 0);
    allocs++;
    return (long)s * UNIT;
}

static void h_free(unsigned off) {
    unsigned s = off / UNIT, len = head_sz[s];
    CHECK(len > 0 && !is_free[s]);
    frees++;
    if (s + len < NU && is_free[s + len]) {
        unsigned nx = s + len;
        heap_remove(nx);
        len += head_sz[nx]; head_sz[nx] = 0; tail_sz[nx - 1] = 0;
        merges++;
    }
    if (s > 0 && is_free[s - tail_sz[s - 1]] && tail_sz[s - 1] > 0) {
        unsigned ps = s - tail_sz[s - 1];
        CHECK(is_free[ps]);
        heap_remove(ps);
        head_sz[s] = 0; tail_sz[s - 1] = 0;
        len += head_sz[ps]; s = ps;
        merges++;
    }
    set_block(s, len, 1);
    heap_push(s);
}

static void heap_check(unsigned *nfree, unsigned *free_units, unsigned *largest) {
    unsigned s = 0, nf = 0, fu = 0, big = 0;
    int prev_free = 0;
    while (s < NU) {
        unsigned len = head_sz[s];
        CHECK(len > 0 && s + len <= NU && tail_sz[s + len - 1] == len);
        if (is_free[s]) {
            CHECK(!prev_free && hpos[s] >= 0 && mh[hpos[s]] == s);
            nf++; fu += len; if (len > big) big = len; prev_free = 1;
        } else { CHECK(hpos[s] == -1); prev_free = 0; }
        s += len;
    }
    CHECK(s == NU && (int)nf == hn);
    for (int i = 1; i < hn; i++) CHECK(!better(mh[i], mh[(i - 1) / 2]));
    *nfree = nf; *free_units = fu; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[900];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fu, big;
    for (int step = 0; step < 12000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 900)) {
            size_t n = 1 + rnd() % 96;
            if (rnd() % 20 == 0) n += 200 + rnd() % 400;
            long off = h_alloc(n);
            if (off < 0) { fails++; continue; }
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            h_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 100 == 0) {
            heap_check(&nf, &fu, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fu, &big);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, fails, nlive);
    printf("splits=%lu merges=%lu heap moves=%lu\n", splits, merges, heap_moves);
    printf("free blocks=%u free units=%u largest=%u\n", nf, fu, big);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fu, &big);
    CHECK(nf == 1 && big == NU && hn == 1);
    printf("drained: single block of %u units\n", big);
    return 0;
}
