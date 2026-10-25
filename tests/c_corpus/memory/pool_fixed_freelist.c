/*
 * title: Fixed-size pool with intrusive free list and misuse detection
 * topic: memory
 * covers: pool allocator, intrusive free list, double free detection, foreign pointer rejection, poison on free, free list walk invariant
 * deps: libc
 */
#define SEED 0xF00DFACE12345ULL
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

#define SLOT 48
#define NSLOTS 100

static _Alignas(16) unsigned char store[SLOT * NSLOTS];
static uint32_t free_head;
static unsigned char allocated[NSLOTS];
static int in_use, peak_use;
static int n_double, n_foreign, n_misaligned, n_exhaust;
#define END 0xFFFFFFFFu

static void pool_init(void) {
    for (uint32_t i = 0; i < NSLOTS; i++) {
        uint32_t nx = i + 1 < NSLOTS ? i + 1 : END;
        memset(store + i * SLOT, 0xA5, SLOT);
        memcpy(store + i * SLOT, &nx, sizeof nx);
    }
    free_head = 0;
}

static void *pool_alloc(void) {
    if (free_head == END) { n_exhaust++; return NULL; }
    uint32_t i = free_head, nx;
    memcpy(&nx, store + i * SLOT, sizeof nx);
    free_head = nx;
    allocated[i] = 1;
    in_use++;
    if (in_use > peak_use) peak_use = in_use;
    /* poison must be intact: nothing wrote to a free slot */
    for (size_t k = 4; k < SLOT; k++) CHECK(store[i * SLOT + k] == 0xA5);
    memset(store + i * SLOT, 0, SLOT);
    return store + i * SLOT;
}

/* returns 0 on success, otherwise an error code */
enum { OK, ERR_FOREIGN, ERR_MISALIGNED, ERR_DOUBLE };
static const char *err_name[] = { "ok", "foreign", "misaligned", "double-free" };

static int pool_free(void *p) {
    unsigned char *c = p;
    if (c < store || c >= store + sizeof store) { n_foreign++; return ERR_FOREIGN; }
    size_t off = (size_t)(c - store);
    if (off % SLOT) { n_misaligned++; return ERR_MISALIGNED; }
    uint32_t i = (uint32_t)(off / SLOT);
    if (!allocated[i]) { n_double++; return ERR_DOUBLE; }
    allocated[i] = 0;
    in_use--;
    memset(c, 0xA5, SLOT);
    memcpy(c, &free_head, sizeof free_head);
    free_head = i;
    return OK;
}

static void pool_check(void) {
    int n = 0;
    unsigned char seen[NSLOTS] = {0};
    for (uint32_t i = free_head; i != END; ) {
        CHECK(i < NSLOTS && !seen[i] && !allocated[i]);
        seen[i] = 1; n++;
        memcpy(&i, store + i * SLOT, sizeof i);
    }
    CHECK(n == NSLOTS - in_use);
}

typedef struct { unsigned char *p; unsigned tag; } Rec;

int main(void) {
    pool_init();
    Rec live[NSLOTS];
    int nlive = 0;
    unsigned tag = 1;
    int bad_results[4] = {0};
    unsigned char outside[SLOT];

    for (int step = 0; step < 5000; step++) {
        unsigned op = rnd() % 100;
        if (op < 52) {
            unsigned char *p = pool_alloc();
            if (p) {
                pat_fill(p, SLOT, tag);
                live[nlive].p = p; live[nlive].tag = tag++;
                nlive++;
            }
        } else if (op < 92) {
            if (nlive) {
                int i = (int)(rnd() % (unsigned)nlive);
                CHECK(pat_ok(live[i].p, SLOT, live[i].tag));
                CHECK(pool_free(live[i].p) == OK);
                /* immediately freeing again must be caught */
                if (op % 5 == 0) bad_results[pool_free(live[i].p)]++;
                live[i] = live[--nlive];
            }
        } else if (op < 96) {
            bad_results[pool_free(outside)]++;
        } else if (nlive) {
            int i = (int)(rnd() % (unsigned)nlive);
            bad_results[pool_free(live[i].p + 1 + rnd() % (SLOT - 1))]++;
        }
        if (step % 100 == 0) {
            pool_check();
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(live[i].p, SLOT, live[i].tag));
        }
    }
    pool_check();
    for (int i = 0; i < nlive; i++) CHECK(pool_free(live[i].p) == OK);
    CHECK(in_use == 0);
    pool_check();
    for (int e = 1; e < 4; e++) printf("rejected %-11s %d\n", err_name[e], bad_results[e]);
    printf("counters: double=%d foreign=%d misaligned=%d\n", n_double, n_foreign, n_misaligned);
    printf("peak in use=%d of %d, exhausted %d times\n", peak_use, NSLOTS, n_exhaust);
    /* after everything is freed the LIFO free list hands out slots in reverse free order */
    void *a = pool_alloc(), *b = pool_alloc();
    CHECK(a && b && a != b);
    printf("slot indices after drain: %d %d\n", (int)(((unsigned char *)a - store) / SLOT), (int)(((unsigned char *)b - store) / SLOT));
    pool_free(a); pool_free(b);
    return 0;
}
