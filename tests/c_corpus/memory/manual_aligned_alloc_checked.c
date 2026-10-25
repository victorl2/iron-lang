/*
 * title: Manual aligned allocation with overflow checks and validation
 * topic: memory
 * covers: over-allocate and round up, stored offset header, power-of-two validation, overflow guards, aligned free
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout: [raw ... pad ... | size_t offset_back | user aligned block]. The word just before the user block stores the distance to raw. */
static long live_aligned;

static int is_pow2(size_t x) { return x && !(x & (x - 1)); }

static void *aligned_alloc_checked(size_t align, size_t size) {
    if (!is_pow2(align)) return NULL;
    if (align < sizeof(size_t)) align = sizeof(size_t);
    size_t extra = align - 1 + sizeof(size_t);
    if (size > SIZE_MAX - extra) return NULL;         /* overflow guard */
    unsigned char *raw = malloc(size + extra);
    if (!raw) return NULL;
    uintptr_t base = (uintptr_t)raw + sizeof(size_t);
    uintptr_t user = (base + (align - 1)) & ~(uintptr_t)(align - 1);
    size_t back = (size_t)(user - (uintptr_t)raw);
    memcpy((unsigned char *)user - sizeof back, &back, sizeof back);
    live_aligned++;
    return (void *)user;
}

static void aligned_free(void *p) {
    if (!p) return;
    size_t back;
    memcpy(&back, (unsigned char *)p - sizeof back, sizeof back);
    free((unsigned char *)p - back);
    live_aligned--;
}

/* checks the invariants that any caller may rely on */
static int aligned_ok(const void *p, size_t align) { return ((uintptr_t)p & (align - 1)) == 0; }

int main(void) {
    size_t aligns[] = {1, 2, 8, 16, 32, 64, 256, 4096};
    size_t sizes[] = {0, 1, 7, 64, 1000};
    int good = 0;
    for (size_t i = 0; i < sizeof aligns / sizeof aligns[0]; i++)
        for (size_t j = 0; j < sizeof sizes / sizeof sizes[0]; j++) {
            unsigned char *p = aligned_alloc_checked(aligns[i], sizes[j]);
            if (!p) { fprintf(stderr, "alloc failed\n"); return 1; }
            size_t eff = aligns[i] < sizeof(size_t) ? sizeof(size_t) : aligns[i];
            if (!aligned_ok(p, eff)) { fprintf(stderr, "misaligned a=%zu\n", aligns[i]); return 1; }
            memset(p, 0x5A, sizes[j]); /* whole user region is writable */
            for (size_t k = 0; k < sizes[j]; k++) if (p[k] != 0x5A) return 1;
            aligned_free(p);
            good++;
        }
    printf("aligned %d allocations across %zu alignments, live=%ld\n", good, sizeof aligns / sizeof aligns[0], live_aligned);

    /* rejected requests */
    printf("align 3: %s\n", aligned_alloc_checked(3, 16) ? "served" : "refused");
    printf("align 0: %s\n", aligned_alloc_checked(0, 16) ? "served" : "refused");
    printf("size SIZE_MAX: %s\n", aligned_alloc_checked(64, SIZE_MAX) ? "served" : "refused");
    printf("size SIZE_MAX-70 align 64: %s\n", aligned_alloc_checked(64, SIZE_MAX - 70) ? "served" : "refused");

    /* SIMD-style use: array of 16-float vectors on 64-byte boundaries */
    float *v = aligned_alloc_checked(64, 10 * 16 * sizeof(float));
    if (!v) return 1;
    int aligned_rows = 0;
    for (int r = 0; r < 10; r++) {
        aligned_rows += aligned_ok(v + r * 16, 64);
        for (int c = 0; c < 16; c++) v[r * 16 + c] = (float)(r * 16 + c);
    }
    float sum = 0;
    for (int i = 0; i < 160; i++) sum += v[i];
    printf("10 rows on 64-byte boundaries: %d, sum=%.1f\n", aligned_rows, (double)sum);
    aligned_free(v);

    /* many interleaved blocks with distinct alignments, verifying no overlap by pattern */
    unsigned char *blk[12];
    size_t bsz[12];
    for (int i = 0; i < 12; i++) {
        bsz[i] = 50 + (size_t)i * 13;
        blk[i] = aligned_alloc_checked((size_t)1 << (i % 7), bsz[i]);
        if (!blk[i]) return 1;
        memset(blk[i], i + 1, bsz[i]);
    }
    int clean = 1;
    for (int i = 0; i < 12; i++) for (size_t k = 0; k < bsz[i]; k++) if (blk[i][k] != i + 1) clean = 0;
    printf("12 mixed blocks pattern intact: %d\n", clean);
    for (int i = 0; i < 12; i++) aligned_free(blk[i]);
    printf("live=%ld\n", live_aligned);
    return (clean && live_aligned == 0) ? 0 : 1;
}
