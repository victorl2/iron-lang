/*
 * title: CLOCK and generalized CLOCK page replacement
 * topic: data_structures
 * covers: CLOCK, second chance, reference bit and counters, circular frame array, hand sweep, queue-rotation oracle, hit ratio comparison
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F 16
#define PAGES 96

static unsigned long long rs = 0xC10C4B17ULL * 3;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct {
    int page[F], cnt[F], used;
    int hand;
    int frame_of[PAGES];
    int maxcnt, incr;           /* incr=0: set to maxcnt on hit (classic bit), incr=1: increment up to maxcnt */
    long hits, sweeps;
} Clock;

static void clock_init(Clock *c, int maxcnt, int incr) {
    memset(c, 0, sizeof *c); c->maxcnt = maxcnt; c->incr = incr;
    for (int i = 0; i < PAGES; i++) c->frame_of[i] = -1;
}
static int clock_access(Clock *c, int pg) {
    int f = c->frame_of[pg];
    if (f >= 0) {
        c->hits++;
        c->cnt[f] = c->incr ? (c->cnt[f] < c->maxcnt ? c->cnt[f] + 1 : c->maxcnt) : c->maxcnt;
        return 1;
    }
    if (c->used < F) { f = c->used++; }
    else {
        while (c->cnt[c->hand] > 0) { c->cnt[c->hand]--; c->hand = (c->hand + 1) % F; c->sweeps++; }
        f = c->hand; c->hand = (c->hand + 1) % F;
        c->frame_of[c->page[f]] = -1;
    }
    c->page[f] = pg; c->cnt[f] = 0; c->frame_of[pg] = f;
    return 0;
}

/* oracle: FIFO queue where entries with a positive counter are decremented and re-queued */
typedef struct { int q[F], cnt[F], n; int maxcnt, incr; long hits; } Queue;
static void q_init(Queue *q, int maxcnt, int incr) { memset(q, 0, sizeof *q); q->maxcnt = maxcnt; q->incr = incr; }
static int q_access(Queue *q, int pg) {
    for (int i = 0; i < q->n; i++) if (q->q[i] == pg) {
        q->hits++;
        q->cnt[i] = q->incr ? (q->cnt[i] < q->maxcnt ? q->cnt[i] + 1 : q->maxcnt) : q->maxcnt;
        return 1;
    }
    if (q->n == F) {
        for (;;) {
            int p0 = q->q[0], c0 = q->cnt[0];
            memmove(&q->q[0], &q->q[1], (size_t)(F - 1) * sizeof(int));
            memmove(&q->cnt[0], &q->cnt[1], (size_t)(F - 1) * sizeof(int));
            if (c0 > 0) { q->q[F - 1] = p0; q->cnt[F - 1] = c0 - 1; }
            else { q->q[F - 1] = pg; q->cnt[F - 1] = 0; break; }
        }
    } else { q->q[q->n] = pg; q->cnt[q->n] = 0; q->n++; }
    return 0;
}
/* FIFO baseline */
static int fifo[F], fn, fhead; static long fhits;
static void fifo_access(int pg) {
    for (int i = 0; i < fn; i++) if (fifo[i] == pg) { fhits++; return; }
    if (fn < F) fifo[fn++] = pg; else { fifo[fhead] = pg; fhead = (fhead + 1) % F; }
}

static int gen(int phase) {
    unsigned r = rnd() % 100;
    if (phase == 0) return r < 80 ? (int)(rnd() % 12) : 12 + (int)(rnd() % 84);
    if (phase == 1) { static int loop; loop = (loop + 1) % 20; return r < 30 ? (int)(rnd() % 8) : loop; }   /* loop slightly bigger than cache */
    return (int)(rnd() % PAGES);
}

int main(void) {
    struct { const char *name; int maxcnt, incr; } cfg[3] = { {"CLOCK", 1, 0}, {"GCLOCK-3", 3, 1}, {"GCLOCK-7", 7, 1} };
    Clock c[3]; Queue q[3];
    for (int i = 0; i < 3; i++) { clock_init(&c[i], cfg[i].maxcnt, cfg[i].incr); q_init(&q[i], cfg[i].maxcnt, cfg[i].incr); }
    long ph[3][3]; memset(ph, 0, sizeof ph); long phf[3] = {0, 0, 0};
    for (int phase = 0; phase < 3; phase++) {
        long prev[3], pf = fhits;
        for (int i = 0; i < 3; i++) prev[i] = c[i].hits;
        for (int s = 0; s < 8000; s++) {
            int pg = gen(phase);
            for (int i = 0; i < 3; i++) {
                int a = clock_access(&c[i], pg), b = q_access(&q[i], pg);
                check(a == b, "clock agrees with queue oracle");
            }
            fifo_access(pg);
        }
        for (int i = 0; i < 3; i++) ph[phase][i] = c[i].hits - prev[i];
        phf[phase] = fhits - pf;
    }
    printf("%-9s %6s %6s %6s\n", "policy", "hot", "loop", "random");
    for (int i = 0; i < 3; i++) {
        check(c[i].hits == q[i].hits, "hit totals");
        printf("%-9s %6ld %6ld %6ld  sweep steps=%ld\n", cfg[i].name, ph[0][i], ph[1][i], ph[2][i], c[i].sweeps);
    }
    printf("%-9s %6ld %6ld %6ld\n", "FIFO", phf[0], phf[1], phf[2]);
    /* resident sets agree */
    for (int i = 0; i < 3; i++) for (int k = 0; k < F; k++) check(c[i].frame_of[q[i].q[k]] >= 0, "resident page");
    return 0;
}
