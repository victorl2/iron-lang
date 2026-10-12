/*
 * title: CSMA/CD with binary exponential backoff
 * topic: networking
 * covers: slotted Ethernet model, collision detection, truncated binary exponential backoff, 16-attempt discard, capture effect, channel efficiency
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned rng_state = 1u;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

enum { MAXN = 40, FRAME_SLOTS = 12, JAM_SLOTS = 2, MAX_ATTEMPTS = 16, BACKOFF_CAP = 10 };

typedef struct { long backoff; int attempts; long succ, drops, collisions; } St;

typedef struct {
    long total, idle, busy, collision_slots, collisions, frames, discarded;
    long per_station_min, per_station_max;
    int max_attempts_seen;
    long attempts_hist[MAX_ATTEMPTS + 1];
} Res;

static void simulate(int n, long horizon, unsigned seed, Res *r, St *st) {
    memset(r, 0, sizeof *r);
    memset(st, 0, sizeof(St) * MAXN);
    rng_state = seed;
    long slot = 0;
    long busy_left = 0;
    while (slot < horizon) {
        if (busy_left > 0) {
            /* backoff timers keep running while the medium is busy; expired stations transmit when it goes idle */
            for (int i = 0; i < n; i++) if (st[i].backoff > 0) st[i].backoff--;
            busy_left--; r->busy++; slot++;
            continue;
        }
        int tx[MAXN], nt = 0;
        for (int i = 0; i < n; i++) if (st[i].backoff == 0) tx[nt++] = i;
        if (nt == 0) {
            for (int i = 0; i < n; i++) if (st[i].backoff > 0) st[i].backoff--;
            r->idle++; slot++;
        } else if (nt == 1) {
            int i = tx[0];
            st[i].succ++;
            r->frames++;
            r->attempts_hist[st[i].attempts + 1]++;
            st[i].attempts = 0;
            st[i].backoff = 0;
            busy_left = FRAME_SLOTS - 1;
            r->busy++; slot++;
        } else {
            r->collisions++;
            r->collision_slots += 1 + JAM_SLOTS;
            slot += 1 + JAM_SLOTS;
            for (int k = 0; k < nt; k++) {
                St *s = &st[tx[k]];
                s->attempts++;
                s->collisions++;
                if (s->attempts > r->max_attempts_seen) r->max_attempts_seen = s->attempts;
                if (s->attempts >= MAX_ATTEMPTS) {
                    s->drops++; r->discarded++;
                    s->attempts = 0; s->backoff = 0; /* frame dropped, next frame starts fresh */
                } else {
                    int e = s->attempts < BACKOFF_CAP ? s->attempts : BACKOFF_CAP;
                    unsigned v = rnd();
                    s->backoff = (long)(v % (1u << e));
                }
            }
        }
    }
    r->total = slot;
    r->per_station_min = st[0].succ; r->per_station_max = st[0].succ;
    for (int i = 1; i < n; i++) {
        if (st[i].succ < r->per_station_min) r->per_station_min = st[i].succ;
        if (st[i].succ > r->per_station_max) r->per_station_max = st[i].succ;
    }
}

int main(void) {
    static St st[MAXN];
    Res r;
    int ns[] = {1, 2, 4, 8, 16, 32};
    long prev_eff = 1000;
    printf("%3s %7s %6s %7s %8s %6s %10s\n", "N", "frames", "coll", "dropped", "eff/1000", "maxatt", "min..max/st");
    for (int k = 0; k < 6; k++) {
        simulate(ns[k], 200000, 7000u + (unsigned)ns[k], &r, st);
        long eff = r.frames * FRAME_SLOTS * 1000 / r.total;
        long fr = 0;
        for (int i = 0; i < ns[k]; i++) fr += st[i].succ;
        check(fr == r.frames, "frames sum over stations");
        check(r.busy + r.idle + r.collision_slots >= r.total - JAM_SLOTS - 1 && r.busy + r.idle + r.collision_slots <= r.total + 3,
              "slot accounting");
        if (ns[k] == 1) check(r.collisions == 0 && eff >= 990, "single station never collides");
        else check(eff < prev_eff + 30, "efficiency does not grow with more contenders");
        prev_eff = eff;
        printf("%3d %7ld %6ld %7ld %8ld %6d %5ld..%ld\n", ns[k], r.frames, r.collisions, r.discarded, eff, r.max_attempts_seen,
               r.per_station_min, r.per_station_max);
    }
    /* capture effect: with two stations the one that just won tends to win again, so shares are lopsided in the short run */
    long lopsided = 0;
    for (int w = 0; w < 40; w++) {
        simulate(2, 600, 100u + (unsigned)w, &r, st);
        long a = st[0].succ, b = st[1].succ;
        long hi = a > b ? a : b, lo = a > b ? b : a;
        if (hi > 2 * lo) lopsided++;
    }
    printf("two stations, 40 short runs: %ld runs where one station got more than twice the other's frames\n", lopsided);
    /* number of attempts needed per delivered frame under heavy contention */
    simulate(16, 200000, 7016u, &r, st);
    printf("N=16 attempts needed per delivered frame:");
    for (int a = 1; a <= 8; a++) printf(" %d:%ld", a, r.attempts_hist[a]);
    printf("\n");
    return 0;
}
