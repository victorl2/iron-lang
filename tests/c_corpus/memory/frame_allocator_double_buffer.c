/*
 * title: Double-buffered frame allocator plus persistent arena
 * topic: memory
 * covers: per-frame arenas, two-frame lifetime, buffer swap and reset, poison verification of reset buffer, persistent arena, per-frame budget stats
 * deps: libc
 */
#define SEED 0xF4A3E5ULL
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

#define FRAME_CAP 2048u
#define PERSIST_CAP 1024u
#define NFRAMES 80

typedef struct { _Alignas(16) unsigned char mem[FRAME_CAP]; unsigned used, peak, allocs; } FrameBuf;
static FrameBuf fb[2];
static _Alignas(16) unsigned char persist[PERSIST_CAP];
static unsigned persist_used;
static int cur;                          /* index of the buffer receiving allocations this frame */

static void *frame_alloc(size_t n) {
    FrameBuf *b = &fb[cur];
    size_t start = (b->used + 7u) & ~7u;
    if (start + n > FRAME_CAP) return NULL;
    b->used = (unsigned)(start + n);
    if (b->used > b->peak) b->peak = b->used;
    b->allocs++;
    return b->mem + start;
}
static void *persist_alloc(size_t n) {
    size_t start = (persist_used + 7u) & ~7u;
    if (start + n > PERSIST_CAP) return NULL;
    persist_used = (unsigned)(start + n);
    return persist + start;
}
/* end of frame: swap; the buffer that becomes current is wiped (it held frame N-2 data) */
static void end_frame(void) {
    cur ^= 1;
    memset(fb[cur].mem, 0xDD, FRAME_CAP);
    fb[cur].used = 0;
}

typedef struct { unsigned char *p; size_t n; unsigned tag; int frame; } Rec;

int main(void) {
    memset(fb[0].mem, 0xDD, FRAME_CAP);
    memset(fb[1].mem, 0xDD, FRAME_CAP);
    static Rec prev[300], now[300];
    Rec keep[16];
    int nprev = 0, nnow = 0, nkeep = 0;
    unsigned tag = 1, total_frame_allocs = 0, dropped = 0, carried = 0, persisted_fails = 0;
    unsigned peak_frame[NFRAMES];
    for (int f = 0; f < NFRAMES; f++) {
        /* load varies by frame: spikes every 16th frame push against the budget */
        int load = 20 + (int)(rnd() % 40) + (f % 16 == 15 ? 120 : 0);
        nnow = 0;
        for (int i = 0; i < load; i++) {
            size_t n = 1 + rnd() % 28;
            unsigned char *p = frame_alloc(n);
            if (!p) { dropped++; continue; }
            pat_fill(p, n, tag);
            now[nnow].p = p; now[nnow].n = n; now[nnow].tag = tag++; now[nnow].frame = f;
            nnow++; total_frame_allocs++;
            /* every so often the game wants a copy that survives: it goes to persistent storage */
            if (rnd() % 40 == 0 && nkeep < 16) {
                unsigned char *q = persist_alloc(n);
                if (!q) persisted_fails++;
                else { memcpy(q, p, n); keep[nkeep].p = q; keep[nkeep].n = n; keep[nkeep].tag = now[nnow - 1].tag; keep[nkeep].frame = f; nkeep++; }
            }
        }
        /* everything from the previous frame is still intact while this frame runs */
        for (int i = 0; i < nprev; i++) { CHECK(pat_ok(prev[i].p, prev[i].n, prev[i].tag)); carried++; }
        for (int i = 0; i < nnow; i++) CHECK(pat_ok(now[i].p, now[i].n, now[i].tag));
        for (int i = 0; i < nkeep; i++) CHECK(pat_ok(keep[i].p, keep[i].n, keep[i].tag));
        /* the other buffer's unused tail is still poison-free of this frame's writes */
        peak_frame[f] = fb[cur].used;
        end_frame();
        /* the buffer just reset must be entirely poison */
        for (unsigned k = 0; k < FRAME_CAP; k++) CHECK(fb[cur].mem[k] == 0xDD);
        memcpy(prev, now, sizeof(Rec) * (size_t)nnow);
        nprev = nnow;
    }
    unsigned mx = 0, mn = ~0u, spikes = 0;
    for (int f = 0; f < NFRAMES; f++) { if (peak_frame[f] > mx) mx = peak_frame[f]; if (peak_frame[f] < mn) mn = peak_frame[f]; if (peak_frame[f] > FRAME_CAP * 3 / 4) spikes++; }
    printf("frames=%d frame allocs=%u dropped over budget=%u\n", NFRAMES, total_frame_allocs, dropped);
    printf("previous-frame records verified=%u\n", carried);
    printf("frame usage min=%u max=%u (budget %u), frames above 75%%: %u\n", mn, mx, FRAME_CAP, spikes);
    printf("buffer peaks: A=%u B=%u\n", fb[0].peak, fb[1].peak);
    printf("persistent: %d blocks kept, %u bytes used, %u refused\n", nkeep, persist_used, persisted_fails);
    return 0;
}
