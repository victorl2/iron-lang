/*
 * title: Fibonacci buddy system
 * topic: memory
 * covers: Fibonacci buddies, uneven split into two sizes, role table for buddy lookup, coalescing, internal fragmentation versus power-of-two rounding
 * deps: libc
 */
#define SEED 0xF1B0B0DDULL
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

#define UNIT 16u
#define TOP 12
static const unsigned F[TOP + 1] = { 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377 };
#define NU 377u
#define NONE 0xFFFFFFFFu

static _Alignas(16) unsigned char heap[NU * UNIT];
static signed char ord[NU];            /* order of the block starting at this unit, -1 otherwise */
static unsigned char isfree[NU];
static unsigned char role[NU][TOP + 1]; /* 0 root, 1 large (left) child, 2 small (right) child */
static uint32_t fhead[TOP + 1];         /* unit index of first free block */
static size_t fib_bytes_total, pow2_bytes_total, req_total, splits, merges;

static void link_free(unsigned s, int k) {
    uint32_t pv = NONE, nx = fhead[k];
    memcpy(heap + (size_t)s * UNIT, &pv, 4);
    memcpy(heap + (size_t)s * UNIT + 4, &nx, 4);
    if (nx != NONE) memcpy(heap + (size_t)nx * UNIT, &s, 4);
    fhead[k] = s; ord[s] = (signed char)k; isfree[s] = 1;
}
static void unlink_free(unsigned s, int k) {
    uint32_t pv, nx;
    memcpy(&pv, heap + (size_t)s * UNIT, 4);
    memcpy(&nx, heap + (size_t)s * UNIT + 4, 4);
    if (pv != NONE) memcpy(heap + (size_t)pv * UNIT + 4, &nx, 4); else fhead[k] = nx;
    if (nx != NONE) memcpy(heap + (size_t)nx * UNIT, &pv, 4);
    isfree[s] = 0;
}

static void fb_init(void) {
    memset(ord, -1, sizeof ord);
    for (int k = 0; k <= TOP; k++) fhead[k] = NONE;
    role[0][TOP] = 0;
    link_free(0, TOP);
}

static long fb_alloc(size_t n) {
    unsigned units = (unsigned)((n + UNIT - 1) / UNIT);
    if (n == 0 || units > NU) return -1;
    int k = 0;
    while (F[k] < units) k++;
    int j = k;
    while (j <= TOP && fhead[j] == NONE) j++;
    if (j > TOP) return -1;
    unsigned s = fhead[j];
    unlink_free(s, j);
    while (j > k) {
        /* j >= 2 here because F[j] > F[k] >= 1 means j >= 1; j == 1 only splits into 1+... which cannot happen: F[1]=2 > F[0]=1 */
        if (j < 2) break;
        unsigned lsz = F[j - 1];
        role[s][j - 1] = 1;
        role[s + lsz][j - 2] = 2;
        splits++;
        if (F[j - 2] >= units) {
            link_free(s, j - 1);        /* keep the large part free, continue in the small part */
            s += lsz; j -= 2;
        } else {
            link_free(s + lsz, j - 2);
            j -= 1;
        }
    }
    ord[s] = (signed char)j; isfree[s] = 0;
    size_t p2 = UNIT;
    while (p2 < n) p2 <<= 1;
    fib_bytes_total += (size_t)F[j] * UNIT; pow2_bytes_total += p2; req_total += n;
    return (long)s * UNIT;
}

static void fb_free(unsigned byte_off) {
    unsigned s = byte_off / UNIT;
    int k = ord[s];
    CHECK(k >= 0 && !isfree[s]);
    for (;;) {
        unsigned r = role[s][k];
        if (r == 1) {
            unsigned p = s + F[k];
            if (k < 1 || p >= NU || !isfree[p] || ord[p] != k - 1) break;
            unlink_free(p, k - 1);
            ord[p] = -1;
            k += 1;
        } else if (r == 2) {
            unsigned p = s - F[k + 1];
            if (k + 1 > TOP || !isfree[p] || ord[p] != k + 1) break;
            unlink_free(p, k + 1);
            ord[s] = -1;
            s = p;
            k += 2;
        } else break;
        merges++;
    }
    link_free(s, k);
}

static size_t fb_check(size_t *nfree) {
    size_t pos = 0, fcount = 0, freeu = 0;
    while (pos < NU) {
        int k = ord[pos];
        CHECK(k >= 0 && k <= TOP);
        if (isfree[pos]) { fcount++; freeu += F[k]; }
        pos += F[k];
    }
    CHECK(pos == NU);
    size_t listed = 0;
    for (int k = 0; k <= TOP; k++) {
        uint32_t prev = NONE;
        for (uint32_t b = fhead[k]; b != NONE; ) {
            uint32_t pv, nx;
            CHECK(isfree[b] && ord[b] == k);
            memcpy(&pv, heap + (size_t)b * UNIT, 4); memcpy(&nx, heap + (size_t)b * UNIT + 4, 4);
            CHECK(pv == prev);
            prev = b; b = nx; listed++;
        }
    }
    CHECK(listed == fcount);
    *nfree = fcount;
    return freeu;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    fb_init();
    Rec live[NU];
    int nlive = 0, fails = 0;
    unsigned tag = 1;
    size_t nf;
    size_t order_hist[TOP + 1] = {0};
    for (int step = 0; step < 9000; step++) {
        if (nlive == 0 || (rnd() % 100 < 53 && nlive < (int)NU)) {
            unsigned k = rnd() % 10;
            size_t n = k < 6 ? 1 + rnd() % 70 : k < 9 ? 71 + rnd() % 300 : 371 + rnd() % 1500;
            long off = fb_alloc(n);
            if (off < 0) { fails++; continue; }
            order_hist[ord[off / UNIT]]++;
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            fb_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 111 == 0) {
            fb_check(&nf);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    for (int k = 0; k <= TOP; k++) printf("F[%2d]=%3u units (%4u bytes): %zu blocks handed out\n", k, F[k], F[k] * UNIT, order_hist[k]);
    printf("splits=%zu merges=%zu failed=%d\n", splits, merges, fails);
    printf("block bytes handed out: fibonacci=%zu power-of-two=%zu (requested %zu)\n", fib_bytes_total, pow2_bytes_total, req_total);
    CHECK(fib_bytes_total <= pow2_bytes_total + pow2_bytes_total / 4);
    for (int i = 0; i < nlive; i++) fb_free(live[i].off);
    size_t fu = fb_check(&nf);
    CHECK(nf == 1 && fu == NU && ord[0] == TOP);
    printf("all freed: one block of %zu units\n", fu);
    return 0;
}
