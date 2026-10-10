/*
 * title: Magazine layer with per-CPU loaded/previous magazines and a depot
 * topic: memory
 * covers: Bonwick magazines, loaded and previous magazine swap, depot of full and empty magazines, backing object pool, object conservation invariant, exchange statistics
 * deps: libc
 */
#define SEED 0x4A6A21EULL
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

#define NCPU 4
#define MAG_CAP 8
#define NOBJ 400
#define OBJSZ 32
#define NMAG 30

typedef struct { int n; int obj[MAG_CAP]; } Mag;
typedef struct { Mag *loaded, *prev; unsigned hits_alloc, hits_free, swaps, depot_xchg, backing_alloc, backing_free; } Cpu;

static _Alignas(16) unsigned char objs[NOBJ * OBJSZ];
static int backing_free[NOBJ], nbacking;
static Mag mags[NMAG];
static Mag *depot_full[NMAG], *depot_empty[NMAG];
static int nfull, nempty;
static Cpu cpu[NCPU];
static unsigned long mag_created;

static void backing_init(void) {
    for (int i = 0; i < NOBJ; i++) backing_free[i] = NOBJ - 1 - i;
    nbacking = NOBJ;
}
static int backing_alloc(void) { return nbacking ? backing_free[--nbacking] : -1; }
static void backing_put(int o) { backing_free[nbacking++] = o; }

static Mag *new_empty_mag(void) {
    if (nempty > 0) return depot_empty[--nempty];
    if (mag_created >= NMAG) return NULL;
    Mag *m = &mags[mag_created++];
    m->n = 0;
    return m;
}

static int obj_alloc(int c) {
    Cpu *p = &cpu[c];
    for (;;) {
        if (p->loaded->n > 0) { p->hits_alloc++; return p->loaded->obj[--p->loaded->n]; }
        if (p->prev->n > 0) { Mag *t = p->loaded; p->loaded = p->prev; p->prev = t; p->swaps++; continue; }
        if (nfull > 0) {
            /* trade our empty loaded magazine for a full one from the depot */
            Mag *full = depot_full[--nfull];
            depot_empty[nempty++] = p->loaded;
            p->loaded = full;
            p->depot_xchg++;
            continue;
        }
        p->backing_alloc++;
        return backing_alloc();
    }
}

static void obj_free(int c, int o) {
    Cpu *p = &cpu[c];
    for (;;) {
        if (p->loaded->n < MAG_CAP) { p->loaded->obj[p->loaded->n++] = o; p->hits_free++; return; }
        if (p->prev->n < MAG_CAP) { Mag *t = p->loaded; p->loaded = p->prev; p->prev = t; p->swaps++; continue; }
        Mag *empty = new_empty_mag();
        if (!empty) { backing_put(o); p->backing_free++; return; }   /* out of magazines: object goes back to the slab layer */
        depot_full[nfull++] = p->loaded;
        p->loaded = empty;
        p->depot_xchg++;
    }
}

static void check_conservation(int live) {
    int cached = 0;
    for (int c = 0; c < NCPU; c++) { cached += cpu[c].loaded->n + cpu[c].prev->n; CHECK(cpu[c].loaded->n <= MAG_CAP && cpu[c].prev->n <= MAG_CAP); }
    for (int i = 0; i < nfull; i++) { CHECK(depot_full[i]->n == MAG_CAP); cached += depot_full[i]->n; }
    for (int i = 0; i < nempty; i++) CHECK(depot_empty[i]->n == 0);
    CHECK(cached + nbacking + live == NOBJ);
}

typedef struct { int o; unsigned tag; } Rec;

int main(void) {
    backing_init();
    for (int c = 0; c < NCPU; c++) { cpu[c].loaded = new_empty_mag(); cpu[c].prev = new_empty_mag(); }
    Rec live[NOBJ];
    int nlive = 0, refused = 0;
    unsigned tag = 1;
    unsigned long remote_frees = 0;
    int owner[NOBJ];
    for (int i = 0; i < NOBJ; i++) owner[i] = -1;
    for (int step = 0; step < 30000; step++) {
        int c = (int)(rnd() % NCPU);
        int phase = (step / 5000) % 3;   /* phase 0 balanced, 1 alloc heavy, 2 free heavy */
        unsigned pa = phase == 0 ? 50 : phase == 1 ? 70 : 30;
        if (nlive == 0 || (rnd() % 100 < pa && nlive < NOBJ - 1)) {
            int o = obj_alloc(c);
            if (o < 0) { refused++; continue; }
            CHECK(o >= 0 && o < NOBJ);
            pat_fill(objs + (size_t)o * OBJSZ, OBJSZ, tag);
            live[nlive].o = o; live[nlive].tag = tag++; owner[o] = c; nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(objs + (size_t)live[i].o * OBJSZ, OBJSZ, live[i].tag));
            if (owner[live[i].o] != c) remote_frees++;
            obj_free(c, live[i].o);
            live[i] = live[--nlive];
        }
        if (step % 200 == 0) check_conservation(nlive);
    }
    check_conservation(nlive);
    for (int i = 0; i < nlive; i++) obj_free((int)(rnd() % NCPU), live[i].o);
    check_conservation(0);
    unsigned long ha = 0, hf = 0, sw = 0, dx = 0, ba = 0;
    for (int c = 0; c < NCPU; c++) {
        printf("cpu%d: alloc hits=%5u free hits=%5u swaps=%4u depot exchanges=%3u backing allocs=%3u frees=%3u\n", c, cpu[c].hits_alloc, cpu[c].hits_free, cpu[c].swaps, cpu[c].depot_xchg, cpu[c].backing_alloc, cpu[c].backing_free);
        ha += cpu[c].hits_alloc; hf += cpu[c].hits_free; sw += cpu[c].swaps; dx += cpu[c].depot_xchg; ba += cpu[c].backing_alloc;
    }
    printf("totals: alloc hits=%lu free hits=%lu swaps=%lu exchanges=%lu backing=%lu refused=%d\n", ha, hf, sw, dx, ba, refused);
    printf("magazines created=%lu depot full=%d empty=%d remote frees=%lu\n", mag_created, nfull, nempty, remote_frees);
    return 0;
}
