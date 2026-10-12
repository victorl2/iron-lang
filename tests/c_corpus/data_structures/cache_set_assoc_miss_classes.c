/*
 * title: Set-associative cache geometry sweep with 3C miss classification
 * topic: data_structures
 * covers: cache geometry, associativity sweep, compulsory capacity and conflict misses, fully associative LRU shadow, per-set LRU, address decomposition
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS 64        /* total cache capacity in blocks */
#define ADDRS 40000
#define UNIVERSE 512     /* distinct blocks */

static unsigned long long rs = 0x3C3C3C3CULL * 7;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int trace[ADDRS];

/* n-way cache with S sets: each set is an array with MRU first */
typedef struct { int sets, ways; int *tag; int *cnt; } Cache;
static Cache mk(int ways) {
    Cache c; c.ways = ways; c.sets = BLOCKS / ways;
    c.tag = malloc((size_t)c.sets * (size_t)ways * sizeof(int)); c.cnt = calloc((size_t)c.sets, sizeof(int));
    return c;
}
static void cfree(Cache *c) { free(c->tag); free(c->cnt); }
static int touch(Cache *c, int blk) {
    int s = blk % c->sets, *t = c->tag + s * c->ways, n = c->cnt[s];
    for (int i = 0; i < n; i++) if (t[i] == blk) { memmove(t + 1, t, (size_t)i * sizeof(int)); t[0] = blk; return 1; }
    if (n < c->ways) c->cnt[s]++; else n--;
    memmove(t + 1, t, (size_t)n * sizeof(int)); t[0] = blk;
    return 0;
}

/* seen-before oracle via bitmap for compulsory misses */
static void gen_trace(int kind) {
    int loop = 0;
    for (int i = 0; i < ADDRS; i++) {
        unsigned r = rnd() % 100;
        int a;
        switch (kind) {
        case 0: a = (int)(rnd() % 48); if (r < 10) a = (int)(rnd() % UNIVERSE); break;              /* fits in cache */
        case 1: a = (loop++ % 80); if (r < 5) a = (int)(rnd() % UNIVERSE); break;                   /* loop, capacity-bound */
        case 2: { int hi = (int)(rnd() % 8), lo = (int)(rnd() % 4); a = hi * 64 + lo; break; }                                 /* power-of-two stride: set conflicts */
        default: a = (int)(rnd() % UNIVERSE);
        }
        trace[i] = a % UNIVERSE;
    }
}

int main(void) {
    const char *names[4] = {"resident", "loop80", "stride64", "uniform"};
    int ways_list[5] = {1, 2, 4, 8, BLOCKS};
    for (int kind = 0; kind < 4; kind++) {
        gen_trace(kind);
        unsigned char seen[UNIVERSE]; memset(seen, 0, sizeof seen);
        long compulsory = 0;
        Cache full = mk(BLOCKS); long full_miss = 0;
        for (int i = 0; i < ADDRS; i++) { if (!seen[trace[i]]) { seen[trace[i]] = 1; compulsory++; } if (!touch(&full, trace[i])) full_miss++; }
        long capacity = full_miss - compulsory;
        check(capacity >= 0, "capacity misses nonnegative");
        printf("%-9s compulsory=%3ld capacity=%5ld |", names[kind], compulsory, capacity);
        long prev_conflict = -1;
        for (int wi = 0; wi < 5; wi++) {
            Cache c = mk(ways_list[wi]); long miss = 0;
            for (int i = 0; i < ADDRS; i++) if (!touch(&c, trace[i])) miss++;
            long conflict = miss - full_miss;
            if (wi == 4) check(conflict == 0, "fully associative has no conflicts");
            printf(" %dw:%ld", ways_list[wi] == BLOCKS ? 64 : ways_list[wi], conflict);
            cfree(&c);
            (void)prev_conflict;
        }
        printf(" (conflict misses, may be negative)\n");
        cfree(&full);
    }
    /* address decomposition sanity: rebuild block number from (tag, set, offset) */
    int bad = 0;
    for (int a = 0; a < 4096; a += 7) {
        int off = a % 16, blk = a / 16, set = blk % 8, tag = blk / 8;
        if ((tag * 8 + set) * 16 + off != a) bad++;
    }
    check(bad == 0, "address decomposition");
    printf("address decomposition ok\n");
    return 0;
}
