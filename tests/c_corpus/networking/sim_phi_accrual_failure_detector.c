/*
 * title: Phi accrual failure detector in fixed point
 * topic: networking
 * covers: heartbeat inter-arrival window, adaptive suspicion level phi, thresholds vs fixed timeouts, false suspicion episodes, detection latency after crash
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

enum { WIN = 50, MAXHB = 400, END_MS = 30000, CRASH_MS = 20000, NDET = 6 };

typedef struct { long t; } Arr;
static Arr arr[MAXHB];
static int narr;

static int cmp_arr(const void *a, const void *b) {
    long x = ((const Arr *)a)->t, y = ((const Arr *)b)->t;
    return (x > y) - (x < y);
}

typedef struct {
    long hist[WIN];
    int n, head;
    long sum;
    long last;
} Window;

static void w_add(Window *w, long iv) {
    if (w->n == WIN) w->sum -= w->hist[w->head];
    else w->n++;
    w->hist[w->head] = iv;
    w->head = (w->head + 1) % WIN;
    w->sum += iv;
}
/* phi in thousandths under an exponential inter-arrival model: phi = (elapsed / mean) * log10(e) */
static long phi_milli(const Window *w, long now_ms) {
    if (w->n < 3) return 0;
    long mean = w->sum / w->n;
    if (mean < 1) mean = 1;
    long elapsed = now_ms - w->last;
    return elapsed * 434294 / mean / 1000;
}

typedef struct { const char *name; int kind; long param; } Det; /* kind 0: phi threshold (milli), kind 1: fixed timeout */
typedef struct { int suspect; int episodes; long suspect_ms; long detected_at; } DState;

int main(void) {
    rng_state = 0xf00d1u;
    /* heartbeats every 100 ms; delay 5..20 ms, with a congested period 10000..12000 where delays reach 400 ms */
    for (long s = 0; s < CRASH_MS; s += 100) {
        unsigned r = rnd();
        long delay = 5 + (long)(r % 16);
        if (s >= 10000 && s < 12000) { unsigned q = rnd(); delay += (long)(q % 400); }
        arr[narr++].t = s + delay;
    }
    qsort(arr, (size_t)narr, sizeof(Arr), cmp_arr);
    Det dets[NDET] = {
        {"phi>=1", 0, 1000}, {"phi>=3", 0, 3000}, {"phi>=8", 0, 8000},
        {"timeout 150ms", 1, 150}, {"timeout 300ms", 1, 300}, {"timeout 600ms", 1, 600},
    };
    DState st[NDET];
    memset(st, 0, sizeof st);
    for (int i = 0; i < NDET; i++) st[i].detected_at = -1;
    Window w;
    memset(&w, 0, sizeof w);
    int next = 0;
    long phi_at_crash_probe[3] = {0, 0, 0};
    long max_phi_before = 0;
    for (long t = 0; t <= END_MS; t++) {
        while (next < narr && arr[next].t <= t) {
            if (w.last > 0 || next > 0) w_add(&w, arr[next].t - w.last);
            w.last = arr[next].t;
            next++;
        }
        if (next == 0) continue;
        long phi = phi_milli(&w, t);
        long silent = t - w.last;
        if (t < CRASH_MS && t > 3000 && phi > max_phi_before) max_phi_before = phi;
        if (t == CRASH_MS + 200) phi_at_crash_probe[0] = phi;
        if (t == CRASH_MS + 500) phi_at_crash_probe[1] = phi;
        if (t == CRASH_MS + 1000) phi_at_crash_probe[2] = phi;
        for (int d = 0; d < NDET; d++) {
            int sus = dets[d].kind == 0 ? phi >= dets[d].param : silent > dets[d].param;
            if (t < 1000) sus = 0; /* warm-up */
            if (sus && !st[d].suspect) { if (t < CRASH_MS + 100) st[d].episodes++; }
            if (sus && t < CRASH_MS) st[d].suspect_ms++;
            if (sus && t >= CRASH_MS && st[d].detected_at < 0 && (t >= CRASH_MS + 100)) st[d].detected_at = t;
            st[d].suspect = sus;
        }
    }
    printf("%d heartbeats before the crash at t=%d; congestion period 10000..12000\n", narr, CRASH_MS);
    printf("%-14s %10s %14s %14s\n", "detector", "false alarms", "suspected (ms)", "detected after");
    for (int d = 0; d < NDET; d++) {
        long last_hb = arr[narr - 1].t;
        long lat = st[d].detected_at - last_hb;
        printf("%-14s %10d %14ld %11ld ms\n", dets[d].name, st[d].episodes, st[d].suspect_ms, lat);
        check(st[d].detected_at >= 0, "every detector eventually declares the crashed node dead");
    }
    printf("phi (x1000) after crash: +200ms=%ld +500ms=%ld +1000ms=%ld; max phi seen while alive=%ld\n", phi_at_crash_probe[0],
           phi_at_crash_probe[1], phi_at_crash_probe[2], max_phi_before);
    check(phi_at_crash_probe[0] < phi_at_crash_probe[1] && phi_at_crash_probe[1] < phi_at_crash_probe[2], "phi grows while silence continues");
    check(st[0].episodes >= st[1].episodes && st[1].episodes >= st[2].episodes, "higher threshold, fewer false alarms");
    check(st[3].episodes >= st[4].episodes && st[4].episodes >= st[5].episodes, "longer timeout, fewer false alarms");
    check(st[0].detected_at <= st[1].detected_at && st[1].detected_at <= st[2].detected_at, "higher threshold, slower detection");
    return 0;
}
