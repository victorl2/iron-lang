/*
 * title: Double-ended stack allocator sharing one buffer
 * topic: memory
 * covers: two stacks in one buffer, growth toward each other, collision refusal, per-side markers, aligned allocation from the top end, gap accounting
 * deps: libc
 */
#define SEED 0xDE57ACC5ULL
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

#define CAP 4096u

static _Alignas(16) unsigned char buf[CAP];
static unsigned lo, hi = CAP;            /* lo: next free from the bottom, hi: end of free gap (top side grows down) */
static unsigned peak_used, min_gap = CAP, refused[2], allocs[2], released[2];

static void note(void) {
    unsigned used = lo + (CAP - hi);
    if (used > peak_used) peak_used = used;
    if (hi - lo < min_gap) min_gap = hi - lo;
}

static long alloc_low(size_t n, unsigned align) {
    unsigned p = (lo + align - 1) & ~(align - 1);
    if ((size_t)p + n > hi) { refused[0]++; return -1; }
    lo = p + (unsigned)n;
    allocs[0]++; note();
    return (long)p;
}
/* top side: block sits just below hi; start aligned down */
static long alloc_high(size_t n, unsigned align) {
    if (n > hi) { refused[1]++; return -1; }
    unsigned p = (hi - (unsigned)n) & ~(align - 1);
    if (p < lo) { refused[1]++; return -1; }
    hi = p;
    allocs[1]++; note();
    return (long)p;
}
static void release_low(unsigned mark) { CHECK(mark <= lo); memset(buf + mark, 0xDD, lo - mark); lo = mark; released[0]++; }
static void release_high(unsigned mark) { CHECK(mark >= hi && mark <= CAP); memset(buf + hi, 0xDD, mark - hi); hi = mark; released[1]++; }

typedef struct { unsigned off; size_t n; unsigned tag; unsigned prev; } Rec;

int main(void) {
    Rec side[2][300];
    int cnt[2] = { 0, 0 };
    unsigned mk[2][8];
    int mkc[2][8], nm[2] = { 0, 0 };
    unsigned tag = 1;
    unsigned long phase_refusals[4] = {0}, collisions_at_full = 0;
    for (int step = 0; step < 20000; step++) {
        int phase = step / 5000;
        /* phase 0: balanced; 1: low heavy; 2: high heavy; 3: balanced with big blocks */
        unsigned side_pick = rnd() % 100;
        int s = phase == 1 ? (side_pick < 80 ? 0 : 1) : phase == 2 ? (side_pick < 80 ? 1 : 0) : (side_pick < 50 ? 0 : 1);
        unsigned op = rnd() % 100;
        if (op < 50) {
            size_t n = 1 + rnd() % (phase == 3 ? 400 : 100);
            unsigned prev = s == 0 ? lo : hi;
            unsigned align = 1u << (rnd() % 4);
            long off = s == 0 ? alloc_low(n, align) : alloc_high(n, align);
            if (off < 0) { phase_refusals[phase]++; continue; }
            CHECK(((unsigned)off & (align - 1)) == 0);
            pat_fill(buf + off, n, tag);
            Rec *r = &side[s][cnt[s]++];
            r->off = (unsigned)off; r->n = n; r->tag = tag++; r->prev = prev;
            CHECK(cnt[s] < 300);
        } else if (op < 72) {
            /* pop the most recent block on this side (LIFO) unless a marker pins it */
            if (cnt[s] > 0 && (nm[s] == 0 || mkc[s][nm[s] - 1] < cnt[s])) {
                Rec *r = &side[s][--cnt[s]];
                CHECK(pat_ok(buf + r->off, r->n, r->tag));
                if (s == 0) release_low(r->prev); else release_high(r->prev);
            }
        } else if (op < 80) {
            if (nm[s] < 8) { mk[s][nm[s]] = s == 0 ? lo : hi; mkc[s][nm[s]] = cnt[s]; nm[s]++; }
        } else if (op < 90) {
            if (nm[s] > 0) {
                nm[s]--;
                if (s == 0) release_low(mk[0][nm[s]]); else release_high(mk[1][nm[s]]);
                cnt[s] = mkc[s][nm[s]];
            }
        } else if (op < 92) {
            /* the bottom stack lives long: occasionally a full flush of the high side */
            if (s == 1) { release_high(CAP); cnt[1] = 0; nm[1] = 0; }
        }
        CHECK(lo <= hi);
        if (step % 100 == 0) {
            for (int k = 0; k < 2; k++)
                for (int i = 0; i < cnt[k]; i++) CHECK(pat_ok(buf + side[k][i].off, side[k][i].n, side[k][i].tag));
            if (cnt[0] && cnt[1]) CHECK(side[0][cnt[0] - 1].off + side[0][cnt[0] - 1].n <= side[1][cnt[1] - 1].off);
        }
        if (hi == lo) collisions_at_full++;
    }
    for (int p = 0; p < 4; p++) printf("phase %d refusals=%lu\n", p, phase_refusals[p]);
    printf("allocs low=%u high=%u refused low=%u high=%u\n", allocs[0], allocs[1], refused[0], refused[1]);
    printf("marker releases low=%u high=%u\n", released[0], released[1]);
    printf("peak used=%u of %u, smallest gap=%u, steps with zero gap=%lu\n", peak_used, CAP, min_gap, collisions_at_full);
    printf("final lo=%u hi=%u gap=%u\n", lo, hi, hi - lo);
    release_low(0); release_high(CAP);
    CHECK(lo == 0 && hi == CAP);
    return 0;
}
