/*
 * title: Anonymous mmap region with size-class sub-allocator
 * topic: memory
 * covers: mmap anonymous, bump carving, size-class free lists, overlap audit
 * deps: posix
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define REGION (256u * 1024u)
#define NCLASS 5

static const unsigned class_size[NCLASS] = {16, 32, 64, 128, 256};

typedef struct Free {
    struct Free *next;
} Free;

typedef struct {
    unsigned char *base;
    size_t used;
    size_t cap;
    Free *bins[NCLASS];
    unsigned long carved[NCLASS];
    unsigned long reused[NCLASS];
} Heap;

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int class_of(size_t n) {
    for (int i = 0; i < NCLASS; i++)
        if (n <= class_size[i])
            return i;
    return -1;
}

static void *heap_alloc(Heap *h, size_t n, int *cls_out) {
    int c = class_of(n);
    if (c < 0)
        return NULL;
    *cls_out = c;
    if (h->bins[c]) {
        Free *f = h->bins[c];
        h->bins[c] = f->next;
        h->reused[c]++;
        return f;
    }
    if (h->used + class_size[c] > h->cap)
        return NULL;
    void *p = h->base + h->used;
    h->used += class_size[c];
    h->carved[c]++;
    return p;
}

static void heap_free(Heap *h, void *p, int c) {
    Free *f = p;
    f->next = h->bins[c];
    h->bins[c] = f;
}

typedef struct {
    unsigned char *p;
    int cls;
    unsigned char tag;
    size_t len;
} Live;

static void fill(Live *l) {
    memset(l->p, l->tag, l->len);
}

static int verify(const Live *l) {
    for (size_t i = 0; i < l->len; i++)
        if (l->p[i] != l->tag)
            return 0;
    return 1;
}

int main(void) {
    Heap h;
    memset(&h, 0, sizeof h);
    void *m = mmap(NULL, REGION, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(m != MAP_FAILED, "mmap");
    h.base = m;
    h.cap = REGION;
    /* fresh anonymous memory is zero-filled */
    for (size_t i = 0; i < 4096; i++)
        check(h.base[i] == 0, "zero fill");

    enum { SLOTS = 300 };
    Live live[SLOTS];
    memset(live, 0, sizeof live);
    unsigned long allocs = 0, frees = 0, failures = 0;
    for (int step = 0; step < 6000; step++) {
        uint64_t r = rnd();
        size_t slot = (size_t)(r % SLOTS);
        if (live[slot].p) {
            check(verify(&live[slot]), "contents intact");
            heap_free(&h, live[slot].p, live[slot].cls);
            live[slot].p = NULL;
            frees++;
        } else {
            size_t n = 1 + (size_t)((r >> 20) % 256);
            int c = 0;
            void *p = heap_alloc(&h, n, &c);
            if (!p) {
                failures++;
                continue;
            }
            check(((uintptr_t)p & 15u) == 0, "16-byte alignment");
            live[slot].p = p;
            live[slot].cls = c;
            live[slot].len = class_size[c];
            live[slot].tag = (unsigned char)(1 + (r >> 40) % 250);
            fill(&live[slot]);
            allocs++;
        }
    }
    /* overlap audit: every live block still holds its own tag */
    size_t nlive = 0;
    for (int i = 0; i < SLOTS; i++)
        if (live[i].p) {
            check(verify(&live[i]), "final contents");
            nlive++;
        }
    printf("allocs=%lu frees=%lu failures=%lu live=%zu\n", allocs, frees, failures, nlive);
    unsigned long carved_total = 0, reused_total = 0;
    for (int c = 0; c < NCLASS; c++) {
        printf("class %3u: carved=%lu reused=%lu\n", class_size[c], h.carved[c], h.reused[c]);
        carved_total += h.carved[c];
        reused_total += h.reused[c];
    }
    check(carved_total + reused_total == allocs, "accounting");
    size_t sum = 0;
    for (int c = 0; c < NCLASS; c++)
        sum += h.carved[c] * class_size[c];
    check(sum == h.used, "bump matches carved");
    printf("bytes carved=%zu of %u\n", h.used, REGION);
    /* oversize request is refused */
    int c = 0;
    check(heap_alloc(&h, 257, &c) == NULL, "oversize refused");
    check(munmap(m, REGION) == 0, "munmap");
    return 0;
}
