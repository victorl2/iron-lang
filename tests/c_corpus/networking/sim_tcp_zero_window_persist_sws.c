/*
 * title: Zero window, persist timer and silly window syndrome avoidance
 * topic: networking
 * covers: receiver window advertisement, zero window stall, lost window update deadlock, persist timer with backoff, silly window syndrome, small segment counting
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

enum { MSS = 512, BUF = 2048, TOTAL = 16384, TINY = 64, UPD_LOSS = 60, ONEWAY = 10 };
enum { EV_DATA, EV_ACK, EV_READ, EV_PERSIST, EV_PROBE };

typedef struct {
    int persist, sws;
    /* sender */
    long snd_nxt, snd_una;
    long wnd;                       /* window from the last ACK */
    int persist_gen, persist_armed;
    long persist_int;
    long segs, tiny, probes, probe_replies_lost;
    /* receiver */
    long rcv_nxt, buffered, adv_free, app_total;
    long updates, updates_lost;
    long finish;
} Conn;
static Conn c;

static long min_l(long a, long b) { return a < b ? a : b; }
static long advertised(void) {
    long free = BUF - c.buffered;
    if (c.sws && free < min_l(MSS, BUF / 2)) return 0; /* receiver-side SWS avoidance: do not advertise crumbs */
    return free;
}
static void send_ack(int kind) { /* kind 0: ack of data (reliable), 1: window update, 2: probe reply */
    long w = advertised();
    c.adv_free = w;
    if (kind != 0) {
        unsigned r = rnd();
        if ((int)(r % 100) < UPD_LOSS) { if (kind == 1) c.updates_lost++; else c.probe_replies_lost++; return; }
    }
    ev_push(now + ONEWAY, EV_ACK, 0, (int)c.rcv_nxt, (int)w, 0, 0);
}
static void arm_persist(void) {
    if (!c.persist || c.persist_armed) return;
    c.persist_armed = 1;
    ev_push(now + c.persist_int, EV_PERSIST, 0, ++c.persist_gen, 0, 0, 0);
}
static void try_send(void) {
    for (;;) {
        long remaining = TOTAL - c.snd_nxt;
        if (remaining <= 0) return;
        long usable = c.wnd - (c.snd_nxt - c.snd_una);
        if (usable <= 0) break;
        long seg = min_l(min_l(MSS, usable), remaining);
        if (c.sws && seg < MSS && seg < remaining && seg < BUF / 2) break; /* sender-side SWS avoidance */
        ev_push(now + ONEWAY, EV_DATA, 0, (int)seg, 0, 0, 0);
        c.snd_nxt += seg;
        c.segs++;
        if (seg < TINY) c.tiny++;
    }
    if (TOTAL - c.snd_nxt > 0 && c.snd_nxt == c.snd_una) arm_persist(); /* nothing in flight and no usable window */
}

static void run(int persist, int sws) {
    memset(&c, 0, sizeof c);
    c.persist = persist; c.sws = sws; c.wnd = BUF; c.persist_int = 50; c.adv_free = BUF;
    evn = 0; now = 0;
    rng_state = 0x2e10a1u;
    ev_push(0, EV_READ, 0, 0, 0, 0, 0);
    try_send();
    int done = 0;
    while (evn > 0 && now < 60000) {
        Ev e = ev_pop();
        switch (e.type) {
        case EV_DATA:
            check(c.buffered + e.a <= BUF, "sender never overruns the receive buffer");
            c.buffered += e.a; c.rcv_nxt += e.a;
            send_ack(0);
            break;
        case EV_ACK:
            c.snd_una = e.a; c.wnd = e.b;
            if (c.persist_armed && e.b > 0) { c.persist_armed = 0; c.persist_gen++; c.persist_int = 50; }
            if (c.snd_una == TOTAL) { c.finish = now; done = 1; }
            else try_send();
            break;
        case EV_READ: {
            /* the application is slow, and stalls completely between t=300 and t=1800 */
            if (!(now >= 300 && now < 1800) && c.buffered > 0) {
                long r = min_l(40, c.buffered);
                c.buffered -= r;
                c.app_total += r;
                long free = BUF - c.buffered;
                long grow = free - c.adv_free;
                if (grow > 0 && (!c.sws || grow >= min_l(MSS, BUF / 2)) && c.adv_free < BUF) { c.updates++; send_ack(1); }
            }
            if (c.app_total < TOTAL) ev_push(now + 10, EV_READ, 0, 0, 0, 0, 0);
            break;
        }
        case EV_PERSIST:
            if (e.a != c.persist_gen || !c.persist_armed) break;
            c.probes++;
            ev_push(now + ONEWAY, EV_PROBE, 0, 0, 0, 0, 0);
            c.persist_int = c.persist_int * 2 > 2000 ? 2000 : c.persist_int * 2;
            ev_push(now + c.persist_int, EV_PERSIST, 0, ++c.persist_gen, 0, 0, 0);
            break;
        case EV_PROBE:
            send_ack(2);
            break;
        }
        if (done) break;
    }
    printf("persist=%-3s sws=%-3s: ", persist ? "on" : "off", sws ? "on" : "off");
    if (!done) {
        printf("DEADLOCK at t=%ld with %ld of %d bytes acked (window updates sent %ld, lost %ld)\n", now, c.snd_una, TOTAL, c.updates,
               c.updates_lost);
        return;
    }
    check(c.app_total <= TOTAL && c.rcv_nxt == TOTAL, "all bytes arrived");
    printf("done at t=%5ld segments=%3ld tiny=%3ld probes=%2ld window updates %ld (lost %ld) probe replies lost %ld\n", c.finish, c.segs,
           c.tiny, c.probes, c.updates, c.updates_lost, c.probe_replies_lost);
}

int main(void) {
    run(0, 0);
    long finished_plain = c.snd_una == TOTAL;
    run(0, 1);
    check(finished_plain && c.snd_una != TOTAL, "SWS avoidance without a persist timer deadlocks after a lost window update");
    run(1, 0);
    long t_nosws = c.finish, tiny_nosws = c.tiny, segs_nosws = c.segs;
    check(c.snd_una == TOTAL, "persist timer prevents deadlock");
    run(1, 1);
    check(c.snd_una == TOTAL, "persist with SWS avoidance completes");
    check(c.tiny == 0 || c.tiny < tiny_nosws, "SWS avoidance removes tiny segments");
    check(c.segs < segs_nosws, "SWS avoidance sends fewer segments");
    printf("without SWS avoidance %ld segments (%ld tiny), with it %ld (%ld tiny); finish %ld vs %ld\n", segs_nosws, tiny_nosws, c.segs, c.tiny, t_nosws, c.finish);
    return 0;
}
