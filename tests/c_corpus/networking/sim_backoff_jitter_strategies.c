/*
 * title: Retry backoff with and without jitter under contention
 * topic: networking
 * covers: fixed delay, exponential backoff, full jitter, equal jitter, decorrelated jitter, thundering herd, seeded PRNG, makespan and wasted attempts
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 8192
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

typedef struct { long t, ord; int type, node, a, b, c, d; } Ev;
static Ev evq[EVCAP];
static int evn;
static long now, ordc;
static int ev_less(const Ev *x, const Ev *y) { return x->t != y->t ? x->t < y->t : x->ord < y->ord; }
static void ev_push(long t, int type, int node, int a, int b, int c, int d) {
    if (evn >= EVCAP) fail("event queue overflow");
    Ev e = {t, ordc++, type, node, a, b, c, d};
    int i = evn++;
    evq[i] = e;
    while (i > 0 && ev_less(&evq[i], &evq[(i - 1) / 2])) {
        Ev tmp = evq[i]; evq[i] = evq[(i - 1) / 2]; evq[(i - 1) / 2] = tmp;
        i = (i - 1) / 2;
    }
}
static Ev ev_pop(void) {
    Ev top = evq[0];
    evq[0] = evq[--evn];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < evn && ev_less(&evq[l], &evq[m])) m = l;
        if (r < evn && ev_less(&evq[r], &evq[m])) m = r;
        if (m == i) break;
        Ev tmp = evq[i]; evq[i] = evq[m]; evq[m] = tmp;
        i = m;
    }
    now = top.t;
    return top;
}

enum { NCLIENT = 120, CAP = 4, BASE = 2, CAPD = 256, MAXTICK = 20000 };
enum { S_FIXED, S_EXP, S_FULL, S_EQUAL, S_DECOR, NS };
static const char *sname[NS] = {"fixed(16)", "exponential", "full jitter", "equal jitter", "decorrelated"};
enum { EV_ATTEMPT };

static long rnd_range(long lo, long hi) { /* inclusive */
    if (hi <= lo) return lo;
    unsigned r = rnd();
    return lo + (long)(r % (unsigned long)(hi - lo + 1));
}
static long next_delay(int strat, int attempt, long prev) {
    long exp = BASE;
    for (int i = 0; i < attempt && exp < CAPD; i++) exp *= 2;
    if (exp > CAPD) exp = CAPD;
    switch (strat) {
    case S_FIXED: return 16;
    case S_EXP: return exp;
    case S_FULL: return rnd_range(0, exp);
    case S_EQUAL: return exp / 2 + rnd_range(0, exp / 2);
    default: {
        long hi = prev * 3 > CAPD ? CAPD : prev * 3;
        return rnd_range(BASE, hi);
    }
    }
}

typedef struct { long makespan, attempts, rejected, peak, maxper, p50_done; int over_cap_ticks; } Res;

static Res run(int strat) {
    static int per_tick[MAXTICK + 400];
    static int tries[NCLIENT];
    static long prev_delay[NCLIENT], done_at[NCLIENT];
    memset(per_tick, 0, sizeof per_tick); memset(tries, 0, sizeof tries); memset(done_at, 0, sizeof done_at);
    Res r;
    memset(&r, 0, sizeof r);
    evn = 0; now = 0;
    rng_state = 0x7e57u;
    for (int c = 0; c < NCLIENT; c++) { ev_push(0, EV_ATTEMPT, c, 0, 0, 0, 0); prev_delay[c] = BASE; }
    static int accepted_at_tick[MAXTICK + 400];
    memset(accepted_at_tick, 0, sizeof accepted_at_tick);
    int done = 0;
    while (evn > 0 && done < NCLIENT) {
        Ev e = ev_pop();
        int c = e.node;
        if (now >= MAXTICK) fail("did not finish");
        per_tick[now]++;
        r.attempts++;
        tries[c]++;
        if (accepted_at_tick[now] < CAP) {
            accepted_at_tick[now]++;
            done++; done_at[c] = now;
            if (now > r.makespan) r.makespan = now;
        } else {
            r.rejected++;
            long d = next_delay(strat, tries[c] - 1, prev_delay[c]);
            if (d < 1) d = 1;
            prev_delay[c] = d;
            ev_push(now + d, EV_ATTEMPT, c, 0, 0, 0, 0);
        }
    }
    for (long t = 1; t <= r.makespan; t++) { /* tick 0 is the initial stampede for everyone */
        if (per_tick[t] > r.peak) r.peak = per_tick[t];
        if (per_tick[t] > 2 * CAP) r.over_cap_ticks++;
    }
    for (int c = 0; c < NCLIENT; c++) if (tries[c] > r.maxper) r.maxper = tries[c];
    /* median completion time */
    long tmp[NCLIENT];
    memcpy(tmp, done_at, sizeof tmp);
    for (int i = 1; i < NCLIENT; i++) { long v = tmp[i]; int j = i - 1; while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; } tmp[j + 1] = v; }
    r.p50_done = tmp[NCLIENT / 2];
    check(done == NCLIENT, "all clients eventually succeed");
    return r;
}

int main(void) {
    Res res[NS];
    printf("%d clients, server accepts %d attempts per tick, all start at t=0\n", NCLIENT, CAP);
    printf("%-13s %8s %9s %9s %6s %10s %7s\n", "strategy", "makespan", "attempts", "rejected", "retry-peak", "peak>2*cap", "median");
    for (int s = 0; s < NS; s++) {
        res[s] = run(s);
        printf("%-13s %8ld %9ld %9ld %6ld %10d %7ld\n", sname[s], res[s].makespan, res[s].attempts, res[s].rejected, res[s].peak,
               res[s].over_cap_ticks, res[s].p50_done);
        check(res[s].attempts == NCLIENT + res[s].rejected, "attempts = successes + rejections");
    }
    check(res[S_FULL].attempts < res[S_EXP].attempts, "jitter spreads retries and wastes fewer attempts than plain exponential");
    check(res[S_FULL].attempts < res[S_FIXED].attempts, "full jitter beats fixed delay on wasted work");
    check(res[S_EXP].peak > res[S_FULL].peak, "unjittered clients retry in lockstep");
    check(res[S_DECOR].attempts <= res[S_EXP].attempts, "decorrelated jitter is no worse than exponential");
    long best = res[0].attempts;
    int who = 0;
    for (int s = 1; s < NS; s++) if (res[s].attempts < best) { best = res[s].attempts; who = s; }
    printf("fewest attempts: %s (%ld)\n", sname[who], best);
    return 0;
}
