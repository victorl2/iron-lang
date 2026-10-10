/*
 * title: Allocation statistics with size histogram and accounting invariants
 * topic: memory
 * covers: live and peak accounting, power-of-two size classes, per-class counts, invariant audit, tagged blocks, workload replay
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCLASS 12 /* class k holds sizes in (2^(k-1), 2^k], class 0 = 1 byte, class 11 = >1024 */

typedef struct { size_t n; int cls; uint32_t tag; } Hdr;

static struct {
    long allocs, frees;
    size_t live_bytes, peak_bytes, total_bytes;
    long live_blocks, peak_blocks;
    long per_class_alloc[NCLASS], per_class_live[NCLASS];
} S;

static int class_of(size_t n) {
    int k = 0;
    size_t cap = 1;
    while (cap < n && k < NCLASS - 1) { cap <<= 1; k++; }
    return k;
}

static void *s_alloc(size_t n) {
    Hdr *h = malloc(sizeof(Hdr) + n);
    if (!h) return NULL;
    h->n = n; h->cls = class_of(n); h->tag = 0xB10C0001u;
    S.allocs++; S.live_blocks++; S.live_bytes += n; S.total_bytes += n;
    S.per_class_alloc[h->cls]++; S.per_class_live[h->cls]++;
    if (S.live_bytes > S.peak_bytes) S.peak_bytes = S.live_bytes;
    if (S.live_blocks > S.peak_blocks) S.peak_blocks = S.live_blocks;
    return h + 1;
}

static int s_free(void *p) {
    if (!p) return 1;
    Hdr *h = (Hdr *)p - 1;
    if (h->tag != 0xB10C0001u) return 0;
    h->tag = 0xDEADDEADu;
    S.frees++; S.live_blocks--; S.live_bytes -= h->n; S.per_class_live[h->cls]--;
    free(h);
    return 1;
}

static int audit(void) {
    if (S.allocs - S.frees != S.live_blocks) return 0;
    long sum = 0;
    for (int k = 0; k < NCLASS; k++) { if (S.per_class_live[k] < 0 || S.per_class_live[k] > S.per_class_alloc[k]) return 0; sum += S.per_class_live[k]; }
    if (sum != S.live_blocks) return 0;
    if (S.peak_bytes < S.live_bytes || S.peak_blocks < S.live_blocks) return 0;
    if (S.total_bytes < S.peak_bytes) return 0;
    return 1;
}

static uint32_t rs = 271828u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    void *slots[64] = {0};
    size_t sz[64];
    for (int i = 0; i < 5000; i++) {
        int k = (int)(rnd() % 64);
        if (slots[k]) {
            if (!s_free(slots[k])) return 1;
            slots[k] = NULL;
        } else {
            /* skewed sizes: mostly small, occasional big */
            uint32_t r = rnd();
            size_t n = (r % 10 < 7) ? 1 + r % 48 : (r % 10 < 9) ? 64 + r % 400 : 1000 + r % 3000;
            slots[k] = s_alloc(n);
            if (!slots[k]) return 1;
            sz[k] = n;
            memset(slots[k], 0xC3, n);
        }
        if (i % 250 == 0 && !audit()) { fprintf(stderr, "audit failed at %d\n", i); return 1; }
    }
    printf("allocs=%ld frees=%ld live_blocks=%ld live_bytes=%zu\n", S.allocs, S.frees, S.live_blocks, S.live_bytes);
    printf("peak_blocks=%ld peak_bytes=%zu total_bytes=%zu\n", S.peak_blocks, S.peak_bytes, S.total_bytes);
    printf("class   range        allocated  live\n");
    for (int k = 0; k < NCLASS; k++) {
        char range[24];
        if (k == 0) snprintf(range, sizeof range, "1");
        else if (k == NCLASS - 1) snprintf(range, sizeof range, ">%d", 1 << (k - 1));
        else snprintf(range, sizeof range, "%d-%d", (1 << (k - 1)) + 1, 1 << k);
        printf("%5d   %-11s %9ld %5ld\n", k, range, S.per_class_alloc[k], S.per_class_live[k]);
    }
    if (!audit()) return 1;
    /* double free is caught by the tag on a block we keep alive */
    void *keep = s_alloc(10);
    Hdr *h = (Hdr *)keep - 1;
    h->tag = 0; /* simulate a stomped header */
    printf("free of stomped header accepted: %d\n", s_free(keep));
    h->tag = 0xB10C0001u;
    s_free(keep);
    long remaining = S.live_blocks;
    for (int k = 0; k < 64; k++) if (slots[k]) { (void)sz[k]; s_free(slots[k]); }
    printf("remaining freed: %ld, final live=%ld audit=%d\n", remaining, S.live_blocks, audit());
    return (S.live_blocks == 0 && audit()) ? 0 : 1;
}
