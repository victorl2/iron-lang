/*
 * title: Auditing an allocator for overlap and pattern corruption
 * topic: memory
 * covers: allocation interval invariants, overlap detection, per-block fill patterns, sorted-interval audit, injected allocator bug
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A simple first-fit allocator over a 16 KB arena, with an injectable bug for the auditor to catch. */
#define ARENA 16384
#define NB 256
static unsigned char arena[ARENA];
typedef struct { size_t off, size; int used; } Seg;
static Seg segs[NB];
static int nseg;
static int bug; /* 0 none, 1 = split miscomputes offset (overlap) */

static void init(void) { segs[0].off = 0; segs[0].size = ARENA; segs[0].used = 0; nseg = 1; }

static int a_alloc(size_t n, size_t *off) {
    n = (n + 7) & ~(size_t)7;
    for (int i = 0; i < nseg; i++) {
        if (!segs[i].used && segs[i].size >= n) {
            if (segs[i].size > n && nseg < NB) {
                memmove(&segs[i + 2], &segs[i + 1], (size_t)(nseg - i - 1) * sizeof(Seg));
                segs[i + 1].off = segs[i].off + n - (bug && n > 200 ? 8 : 0); /* bug: remainder starts 8 bytes early */
                segs[i + 1].size = segs[i].size - n;
                segs[i + 1].used = 0;
                segs[i].size = n;
                nseg++;
            }
            segs[i].used = 1;
            *off = segs[i].off;
            return i;
        }
    }
    return -1;
}

static void a_free_at(size_t off) {
    for (int i = 0; i < nseg; i++)
        if (segs[i].used && segs[i].off == off) {
            segs[i].used = 0;
            if (i + 1 < nseg && !segs[i + 1].used) { segs[i].size += segs[i + 1].size; memmove(&segs[i + 1], &segs[i + 2], (size_t)(nseg - i - 2) * sizeof(Seg)); nseg--; }
            if (i > 0 && !segs[i - 1].used) { segs[i - 1].size += segs[i].size; memmove(&segs[i], &segs[i + 1], (size_t)(nseg - i - 1) * sizeof(Seg)); nseg--; }
            return;
        }
}

/* Auditor: works only from what a client observes (offset,size,pattern), not from allocator internals. */
typedef struct { size_t off, size; unsigned char pat; int live; } Client;
static Client cl[128];
static int ncl;

static int audit(const char **why) {
    /* sort live clients by offset (insertion sort on a copy of indexes) and check strictly increasing, non-overlapping, in range */
    int idx[128], n = 0;
    for (int i = 0; i < ncl; i++) if (cl[i].live) idx[n++] = i;
    for (int i = 1; i < n; i++) { int x = idx[i], j = i - 1; while (j >= 0 && cl[idx[j]].off > cl[x].off) { idx[j + 1] = idx[j]; j--; } idx[j + 1] = x; }
    for (int i = 0; i < n; i++) {
        Client *c = &cl[idx[i]];
        if (c->off + c->size > ARENA) { *why = "out of arena"; return 0; }
        if (i && cl[idx[i - 1]].off + cl[idx[i - 1]].size > c->off) { *why = "overlap"; return 0; }
    }
    for (int i = 0; i < n; i++) {
        Client *c = &cl[idx[i]];
        for (size_t k = 0; k < c->size; k++) if (arena[c->off + k] != c->pat) { *why = "pattern corrupted"; return 0; }
    }
    return 1;
}

static int run(int with_bug, unsigned seed, int *steps_out, const char **why) {
    memset(arena, 0, sizeof arena);
    init(); ncl = 0; bug = with_bug;
    unsigned r = seed;
    for (int step = 1; step <= 400; step++) {
        r = r * 1664525u + 1013904223u;
        int do_alloc = ((r >> 24) % 100) < 60;
        if (do_alloc && ncl < 128) {
            size_t n = 16 + (r >> 8) % 300;
            size_t off;
            if (a_alloc(n, &off) >= 0) {
                cl[ncl].off = off; cl[ncl].size = n; cl[ncl].pat = (unsigned char)(step * 7 + 1); cl[ncl].live = 1;
                memset(arena + off, cl[ncl].pat, n);
                ncl++;
            }
        } else {
            int live_n = 0, pick = -1;
            for (int i = 0; i < ncl; i++) if (cl[i].live) live_n++;
            if (live_n) {
                int want = (int)((r >> 4) % (unsigned)live_n);
                for (int i = 0; i < ncl; i++) if (cl[i].live && want-- == 0) { pick = i; break; }
                a_free_at(cl[pick].off);
                cl[pick].live = 0;
            }
        }
        if (!audit(why)) { *steps_out = step; return 0; }
    }
    *steps_out = 400;
    return 1;
}

int main(void) {
    int steps;
    const char *why = "none";
    int ok = run(0, 12345u, &steps, &why);
    printf("healthy allocator: %s after %d steps\n", ok ? "passed" : why, steps);
    if (!ok) return 1;
    int caught = 0, seeds = 6;
    for (unsigned s = 1; s <= (unsigned)seeds; s++) {
        int st;
        const char *w = "none";
        int good = run(1, s * 1000u + 7u, &st, &w);
        printf("buggy allocator seed %u: %s at step %d\n", s, good ? "not detected" : w, st);
        caught += !good;
    }
    printf("caught %d of %d buggy runs\n", caught, seeds);
    return caught == seeds ? 0 : 1;
}
