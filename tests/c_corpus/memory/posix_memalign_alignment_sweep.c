/*
 * title: posix_memalign and aligned_alloc alignment sweep
 * topic: memory
 * covers: posix_memalign, aligned_alloc, power-of-two alignments, overlap audit, pattern integrity
 * deps: posix
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

int main(void) {
    enum { MAXP = 13 };
    void *keep[MAXP + 1];
    size_t sizes[MAXP + 1];
    int n = 0;
    /* posix_memalign at every power of two from pointer size to 4096 */
    for (int e = 3; e <= MAXP; e++) {
        size_t al = (size_t)1 << e;
        size_t sz = al * 3 + 5;
        void *p = NULL;
        int rc = posix_memalign(&p, al, sz);
        check(rc == 0, "posix_memalign");
        check(((uintptr_t)p & (al - 1)) == 0, "aligned");
        memset(p, (int)(0xA0 + e), sz);
        keep[n] = p;
        sizes[n] = sz;
        n++;
        printf("posix_memalign align=%5zu size=%5zu ok\n", al, sz);
    }
    /* patterns from different blocks must not have collided */
    int e = 3;
    for (int i = 0; i < n; i++, e++) {
        unsigned char *b = keep[i];
        for (size_t k = 0; k < sizes[i]; k++)
            check(b[k] == (unsigned char)(0xA0 + e), "pattern intact");
    }
    for (int i = 0; i < n; i++)
        free(keep[i]);

    /* many live blocks at one alignment: none may overlap and all stay aligned */
    enum { MANY = 64 };
    unsigned char *blk[MANY];
    size_t blen[MANY];
    for (int i = 0; i < MANY; i++) {
        void *p = NULL;
        blen[i] = (size_t)(i * 37 % 300) + 1;
        check(posix_memalign(&p, 64, blen[i]) == 0, "posix_memalign 64");
        check(((uintptr_t)p & 63u) == 0, "64-byte aligned");
        blk[i] = p;
        memset(p, i + 1, blen[i]);
    }
    unsigned long overlaps = 0;
    for (int i = 0; i < MANY; i++)
        for (int j = i + 1; j < MANY; j++) {
            uintptr_t a0 = (uintptr_t)blk[i], a1 = a0 + blen[i], b0 = (uintptr_t)blk[j], b1 = b0 + blen[j];
            if (a0 < b1 && b0 < a1)
                overlaps++;
        }
    for (int i = 0; i < MANY; i++)
        for (size_t k = 0; k < blen[i]; k++)
            check(blk[i][k] == (unsigned char)(i + 1), "block contents");
    printf("%d blocks at alignment 64: overlaps=%lu\n", MANY, overlaps);
    check(overlaps == 0, "no overlaps");
    for (int i = 0; i < MANY; i++)
        free(blk[i]);

    /* aligned_alloc with the size a multiple of the alignment (the portable contract) */
    size_t als[] = {16, 32, 64, 128, 256, 1024};
    unsigned long total = 0;
    for (size_t i = 0; i < sizeof als / sizeof als[0]; i++) {
        size_t al = als[i];
        for (size_t mult = 1; mult <= 4; mult++) {
            size_t sz = al * mult;
            unsigned char *p = aligned_alloc(al, sz);
            check(p != NULL, "aligned_alloc");
            check(((uintptr_t)p & (al - 1)) == 0, "aligned_alloc alignment");
            for (size_t k = 0; k < sz; k++)
                p[k] = (unsigned char)(k ^ al);
            unsigned s = 0;
            for (size_t k = 0; k < sz; k++)
                s += p[k];
            total += s;
            free(p);
        }
        printf("aligned_alloc align=%4zu blocks 1..4 ok\n", al);
    }
    printf("aligned_alloc pattern total: %lu\n", total);

    /* zero-size requests: result may be NULL or unique, but must be freeable */
    void *z = NULL;
    int rc = posix_memalign(&z, 64, 0);
    check(rc == 0, "zero size ok");
    free(z);
    printf("zero-size request accepted\n");
    return 0;
}
