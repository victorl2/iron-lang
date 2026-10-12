/*
 * title: Circuit breaker state machine against a flapping backend
 * topic: networking
 * covers: closed/open/half-open states, rolling failure window, open timeout backoff, probe quota, fast-fail accounting, seeded fault injection
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

enum { CLOSED, OPEN, HALF_OPEN };
static const char *sname[] = {"CLOSED", "OPEN", "HALF_OPEN"};
enum { WINDOW = 10, MIN_CALLS = 8, FAIL_PCT = 50, BASE_OPEN = 40, MAX_OPEN = 320, PROBES = 3, HORIZON = 700 };
enum { LAT_OK = 2, LAT_FAIL = 30, LAT_FAST_FAIL = 0 };

typedef struct {
    int state;
    int win[WINDOW], wn, whead;      /* 1 = failure */
    long opened_at, open_for;
    int half_ok, half_inflight;
    long time_in[3], last_change;
    int trips;
    long transitions;
} Breaker;

static void transition(Breaker *b, int to, long t, int log) {
    b->time_in[b->state] += t - b->last_change;
    b->last_change = t;
    if (log) printf("  t=%3ld %-9s -> %s\n", t, sname[b->state], sname[to]);
    b->state = to;
    b->transitions++;
}
static void record(Breaker *b, int failed) {
    b->win[b->whead] = failed;
    b->whead = (b->whead + 1) % WINDOW;
    if (b->wn < WINDOW) b->wn++;
}
static int failure_pct(const Breaker *b) {
    int f = 0;
    for (int i = 0; i < b->wn; i++) f += b->win[i];
    return b->wn ? f * 100 / b->wn : 0;
}

/* backend behaviour by phase: 2% failures, then a full outage, a flaky period, then recovery */
static int backend_fails(long t) {
    unsigned r = rnd();
    int pct = t < 100 ? 2 : t < 220 ? 100 : t < 340 ? 50 : 2;
    return (int)(r % 100) < pct;
}

typedef struct { long calls, backend_calls, rejected, backend_fail, client_fail, lat_total; long outage_calls; } Stats;

static Stats run(int use_breaker, int log) {
    Breaker b;
    memset(&b, 0, sizeof b);
    b.open_for = BASE_OPEN;
    Stats st;
    memset(&st, 0, sizeof st);
    rng_state = 0xb2eaf3u;
    for (long t = 0; t < HORIZON; t++) {
        st.calls++;
        int allow = 1;
        if (use_breaker) {
            if (b.state == OPEN && t - b.opened_at >= b.open_for) { transition(&b, HALF_OPEN, t, log); b.half_ok = 0; b.half_inflight = 0; }
            if (b.state == OPEN) allow = 0;
            else if (b.state == HALF_OPEN) {
                if (b.half_inflight >= PROBES) allow = 0; else b.half_inflight++;
            }
        }
        if (!allow) { st.rejected++; st.client_fail++; st.lat_total += LAT_FAST_FAIL; continue; }
        st.backend_calls++;
        if (t >= 100 && t < 220) st.outage_calls++;
        int failed = backend_fails(t);
        st.lat_total += failed ? LAT_FAIL : LAT_OK;
        if (failed) { st.backend_fail++; st.client_fail++; }
        if (!use_breaker) continue;
        if (b.state == HALF_OPEN) {
            b.half_inflight--;
            if (failed) { /* probe failed: reopen with a longer timeout */
                b.open_for = b.open_for * 2 > MAX_OPEN ? MAX_OPEN : b.open_for * 2;
                b.opened_at = t; b.trips++;
                transition(&b, OPEN, t, log);
            } else if (++b.half_ok >= PROBES) {
                b.open_for = BASE_OPEN; b.wn = 0; b.whead = 0;
                transition(&b, CLOSED, t, log);
            }
        } else if (b.state == CLOSED) {
            record(&b, failed);
            if (b.wn >= MIN_CALLS && failure_pct(&b) >= FAIL_PCT) {
                b.opened_at = t; b.trips++;
                transition(&b, OPEN, t, log);
            }
        }
    }
    b.time_in[b.state] += HORIZON - b.last_change;
    if (use_breaker) {
        printf("  time in state: CLOSED=%ld OPEN=%ld HALF_OPEN=%ld, trips=%d\n", b.time_in[0], b.time_in[1], b.time_in[2], b.trips);
        check(b.time_in[0] + b.time_in[1] + b.time_in[2] == HORIZON, "state time adds up");
    }
    return st;
}

int main(void) {
    printf("with circuit breaker\n");
    Stats with = run(1, 1);
    printf("without circuit breaker\n");
    Stats without = run(0, 0);
    printf("%-22s %8s %8s\n", "", "breaker", "none");
    printf("%-22s %8ld %8ld\n", "backend calls", with.backend_calls, without.backend_calls);
    printf("%-22s %8ld %8ld\n", "fast-failed (rejected)", with.rejected, without.rejected);
    printf("%-22s %8ld %8ld\n", "backend failures", with.backend_fail, without.backend_fail);
    printf("%-22s %8ld %8ld\n", "client-visible errors", with.client_fail, without.client_fail);
    printf("%-22s %8ld %8ld\n", "backend calls in outage", with.outage_calls, without.outage_calls);
    printf("%-22s %8ld %8ld\n", "mean latency x100", with.lat_total * 100 / with.calls, without.lat_total * 100 / without.calls);
    check(with.calls == with.backend_calls + with.rejected, "every call was either sent or rejected");
    check(with.outage_calls * 3 < without.outage_calls, "the breaker shields the failing backend during the outage");
    check(with.lat_total < without.lat_total, "fast failing lowers total latency");
    check(with.rejected > 0 && with.backend_fail < without.backend_fail, "fewer failures reach the backend");
    return 0;
}
