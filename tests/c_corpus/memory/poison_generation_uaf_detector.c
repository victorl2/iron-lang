/*
 * title: Use-after-free detection by poisoning and generations
 * topic: memory
 * covers: poison fill, generation-checked pointers, freed-block sentinel, double free detection, checked accessor macros
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A checked heap: blocks live in a fixed array, so bugs are reported instead of crashing. */
#define NBLK 8
#define BLK_SIZE 24
#define POISON 0xA5

typedef struct {
    unsigned char mem[BLK_SIZE];
    unsigned gen;
    int in_use;
} Block;

typedef struct {
    int idx;
    unsigned gen;
} Ref;

static Block heap[NBLK];
static int errors_uaf, errors_double, errors_oob, errors_poison_read;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Ref h_alloc(void) {
    for (int i = 0; i < NBLK; i++)
        if (!heap[i].in_use) {
            heap[i].in_use = 1;
            memset(heap[i].mem, 0, BLK_SIZE);
            Ref r = {i, heap[i].gen};
            return r;
        }
    Ref none = {-1, 0};
    return none;
}

static int h_free(Ref r) {
    if (r.idx < 0 || !heap[r.idx].in_use || heap[r.idx].gen != r.gen) {
        errors_double++;
        return 0;
    }
    memset(heap[r.idx].mem, POISON, BLK_SIZE);
    heap[r.idx].in_use = 0;
    heap[r.idx].gen++;
    return 1;
}

/* Every access goes through here: returns NULL (and counts the bug) if the ref is stale. */
static unsigned char *h_at(Ref r, int off) {
    if (r.idx < 0 || heap[r.idx].gen != r.gen || !heap[r.idx].in_use) {
        errors_uaf++;
        return NULL;
    }
    if (off < 0 || off >= BLK_SIZE) {
        errors_oob++;
        return NULL;
    }
    return &heap[r.idx].mem[off];
}

/* Raw (unchecked) view used by a "buggy" program: we only count poison reads. */
static int raw_read(int idx, int off) {
    int v = heap[idx].mem[off];
    if (v == POISON)
        errors_poison_read++;
    return v;
}

int main(void) {
    Ref a = h_alloc(), b = h_alloc(), c = h_alloc();
    printf("allocated blocks %d %d %d\n", a.idx, b.idx, c.idx);
    *h_at(a, 0) = 11;
    *h_at(b, 5) = 22;
    printf("a[0]=%d b[5]=%d\n", *h_at(a, 0), *h_at(b, 5));

    /* out-of-bounds write attempt */
    printf("oob at 24: %s\n", h_at(a, 24) ? "allowed" : "rejected");

    /* free b, then use the stale ref */
    printf("free b: %d\n", h_free(b));
    printf("stale read: %s\n", h_at(b, 5) ? "allowed" : "rejected");
    printf("raw read of freed block: 0x%02X\n", raw_read(b.idx, 5));

    /* double free */
    printf("double free: %d\n", h_free(b));

    /* slot reuse: the new ref works, the old one is still rejected (gen differs) */
    Ref d = h_alloc();
    printf("reused slot: %s, gen %u vs old %u\n", d.idx == b.idx ? "yes" : "no", d.gen, b.gen);
    *h_at(d, 5) = 33;
    printf("new ref reads %d, old ref %s\n", *h_at(d, 5), h_at(b, 5) ? "allowed" : "rejected");

    /* freed data is zeroed on alloc, so old contents never leak into the new owner */
    int leak = 0;
    for (int i = 0; i < BLK_SIZE; i++)
        if (i != 5 && *h_at(d, i) != 0)
            leak++;
    printf("bytes leaked into new owner: %d\n", leak);

    /* fill and free everything, check poison pattern on all blocks */
    Ref refs[NBLK];
    int n = 0;
    for (;;) {
        Ref r = h_alloc();
        if (r.idx < 0)
            break;
        refs[n++] = r;
    }
    printf("filled heap with %d more blocks; next alloc %s\n", n, h_alloc().idx < 0 ? "fails" : "ok");
    for (int i = 0; i < n; i++)
        check(h_free(refs[i]) == 1, "free");
    h_free(a);
    h_free(c);
    h_free(d);
    int poisoned = 0;
    for (int i = 0; i < NBLK; i++)
        for (int j = 0; j < BLK_SIZE; j++)
            poisoned += heap[i].mem[j] == POISON;
    printf("poisoned bytes: %d of %d\n", poisoned, NBLK * BLK_SIZE);
    printf("gens:");
    for (int i = 0; i < NBLK; i++)
        printf(" %u", heap[i].gen);
    printf("\nerrors: uaf=%d double=%d oob=%d poison_reads=%d\n", errors_uaf, errors_double,
           errors_oob, errors_poison_read);
    check(errors_uaf == 2 && errors_double == 1 && errors_oob == 1 && errors_poison_read == 1,
          "error counts");
    check(poisoned == NBLK * BLK_SIZE, "all poisoned");
    return 0;
}
