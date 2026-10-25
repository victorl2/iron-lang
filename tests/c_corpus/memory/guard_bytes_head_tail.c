/*
 * title: Head and tail guard bytes detect buffer under and overruns
 * topic: memory
 * covers: guard patterns, canary verification, corruption classification, size stamp, checked free
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 16
#define HEAD_PAT 0xA5
#define TAIL_PAT 0x5A
#define MAGIC 0xC0FFEE11u

/* raw layout: [size_t n][u32 magic][pad][GUARD head][payload n][GUARD tail] */
typedef struct { size_t n; uint32_t magic; uint32_t pad; } Meta;
enum { G_OK = 0, G_UNDERRUN = 1, G_OVERRUN = 2, G_BOTH = 3, G_BADMAGIC = 4 };

static const char *gname(int g) {
    static const char *names[] = {"ok", "underrun", "overrun", "both", "bad-header"};
    return names[g];
}

static void *g_alloc(size_t n) {
    unsigned char *raw = malloc(sizeof(Meta) + GUARD + n + GUARD);
    if (!raw) return NULL;
    Meta m = {n, MAGIC, 0};
    memcpy(raw, &m, sizeof m);
    memset(raw + sizeof m, HEAD_PAT, GUARD);
    memset(raw + sizeof m + GUARD + n, TAIL_PAT, GUARD);
    unsigned char *user = raw + sizeof m + GUARD;
    memset(user, 0xEE, n); /* fresh-memory fill */
    return user;
}

static int g_check(const void *p, size_t *n_out) {
    const unsigned char *user = p;
    const unsigned char *raw = user - GUARD - sizeof(Meta);
    Meta m;
    memcpy(&m, raw, sizeof m);
    if (m.magic != MAGIC) return G_BADMAGIC;
    int r = 0;
    for (int i = 0; i < GUARD; i++) if (raw[sizeof m + i] != HEAD_PAT) { r |= G_UNDERRUN; break; }
    for (int i = 0; i < GUARD; i++) if (user[m.n + i] != TAIL_PAT) { r |= G_OVERRUN; break; }
    if (n_out) *n_out = m.n;
    return r;
}

static int g_free(void *p) {
    int r = g_check(p, NULL);
    if (r == G_BADMAGIC) return r;
    unsigned char *raw = (unsigned char *)p - GUARD - sizeof(Meta);
    memset(raw, 0, sizeof(Meta)); /* kill the magic so a second free is noticed */
    free(raw);
    return r;
}

/* a "buggy" copy routine with a selectable defect */
static void buggy_fill(unsigned char *dst, size_t n, int defect) {
    size_t lo = 0, hi = n;
    if (defect == 1) hi = n + 3;          /* off by three on the high side */
    if (defect == 2) lo = (size_t)0, dst -= 2; /* starts two bytes early */
    if (defect == 3) { hi = n + 2; dst -= 1; }
    for (size_t i = lo; i < hi; i++) dst[i] = (unsigned char)(i + 1);
}

int main(void) {
    static const size_t sizes[] = {1, 7, 16, 33, 100};
    int counts[5] = {0};
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        size_t n = sizes[si];
        printf("size %3zu:", n);
        for (int defect = 0; defect <= 3; defect++) {
            unsigned char *p = g_alloc(n);
            if (!p) return 1;
            buggy_fill(p, n, defect);
            int r = g_check(p, NULL);
            int want = defect == 0 ? G_OK : defect == 1 ? G_OVERRUN : defect == 2 ? G_UNDERRUN : G_BOTH;
            if (defect == 2) {
                /* dst was shifted back by 2 while writing n bytes: tail is untouched */
                want = G_UNDERRUN;
            }
            if (r != want) { fprintf(stderr, "size %zu defect %d: got %s want %s\n", n, defect, gname(r), gname(want)); return 1; }
            counts[r]++;
            printf(" %s", gname(r));
            int fr = g_free(p);
            if (fr != r) return 1;
        }
        printf("\n");
    }
    printf("totals: ok=%d under=%d over=%d both=%d\n", counts[G_OK], counts[G_UNDERRUN], counts[G_OVERRUN], counts[G_BOTH]);

    /* single-byte corruption sweep across the guards of a 20-byte block */
    unsigned char *p = g_alloc(20);
    int detected = 0, total = 0;
    for (int off = -GUARD; off < 20 + GUARD; off++) {
        unsigned char save = p[off];
        p[off] = (unsigned char)(save ^ 0xFF);
        int r = g_check(p, NULL);
        int inside = off >= 0 && off < 20;
        if (inside ? r != G_OK : r == G_OK) { fprintf(stderr, "sweep miss at %d\n", off); return 1; }
        if (!inside) detected++;
        total++;
        p[off] = save;
    }
    printf("guard sweep: %d/%d positions, all %d guard-byte flips detected\n", total, total, detected);
    size_t n = 0;
    int intact = g_check(p, &n);
    printf("intact block check=%s size=%zu\n", gname(intact), n);
    g_free(p);
    return 0;
}
