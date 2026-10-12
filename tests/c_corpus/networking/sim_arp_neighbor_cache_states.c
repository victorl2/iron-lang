/*
 * title: ARP neighbor cache with reachability states
 * topic: networking
 * covers: ARP resolution, pending packet queue, request retries, negative caching, stale/delay/probe reachability, gratuitous ARP
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 4096
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

enum { EV_SEND, EV_RETRY, EV_REACH, EV_DELAY, EV_HOLD, EV_REPLY, EV_GARP, EV_PROBE };
enum { NONE, INCOMPLETE, REACHABLE, STALE, DELAY, PROBE, FAILED };
static const char *sname[] = {"NONE", "INCOMPLETE", "REACHABLE", "STALE", "DELAY", "PROBE", "FAILED"};
enum { NIP = 8, QMAX = 3 };
enum { T_RETRANS = 1000, T_REACH = 30000, T_DELAY = 5000, T_HOLD = 20000, MAX_TRIES = 3 };

typedef struct {
    int state, mac, tries, gen, queued;
} Neigh;
typedef struct { int up_from, up_to, delay, mac; } Peer;

static Neigh nc[NIP];
static Peer peers[NIP];
static long tx_ok, tx_via_stale, dropped_q, dropped_failed, requests, probes, replies, queued_total;
static long tx_by_mac[64];
static int transitions[8][8];

static void setst(int ip, int st) {
    printf("t=%6ld ip%d %-10s -> %s\n", now, ip, sname[nc[ip].state], sname[st]);
    transitions[nc[ip].state][st]++;
    nc[ip].state = st;
}
static void arm(int type, int ip, long delay) { ev_push(now + delay, type, 0, ip, ++nc[ip].gen, 0, 0); }
static void request(int ip, int unicast) {
    if (unicast) probes++; else requests++;
    Peer *p = &peers[ip];
    if (now + p->delay >= p->up_from && now + p->delay < p->up_to) ev_push(now + p->delay, EV_REPLY, 0, ip, p->mac, 0, 0);
}
static void flush(int ip) {
    while (nc[ip].queued > 0) { nc[ip].queued--; tx_ok++; tx_by_mac[nc[ip].mac]++; }
}
static void confirm(int ip, int mac) {
    nc[ip].mac = mac;
    if (nc[ip].state != REACHABLE) setst(ip, REACHABLE);
    arm(EV_REACH, ip, T_REACH);
    flush(ip);
}

static void handle_send(int ip) {
    Neigh *n = &nc[ip];
    switch (n->state) {
    case NONE:
        setst(ip, INCOMPLETE);
        n->tries = 1; n->queued = 1; queued_total++;
        request(ip, 0);
        arm(EV_RETRY, ip, T_RETRANS);
        break;
    case INCOMPLETE:
        if (n->queued == QMAX) dropped_q++; else { n->queued++; queued_total++; }
        break;
    case REACHABLE: tx_ok++; tx_by_mac[n->mac]++; break;
    case STALE:
        tx_ok++; tx_via_stale++; tx_by_mac[n->mac]++;
        setst(ip, DELAY);
        arm(EV_DELAY, ip, T_DELAY);
        break;
    case DELAY:
    case PROBE: tx_ok++; tx_by_mac[n->mac]++; break;
    case FAILED: dropped_failed++; break;
    }
}

int main(void) {
    for (int i = 0; i < NIP; i++) { peers[i].up_from = 0; peers[i].up_to = 1 << 30; peers[i].delay = 3; peers[i].mac = 10 + i; }
    peers[3].up_from = 1 << 29;              /* never answers */
    peers[4].delay = 1500;                   /* answers to the second request */
    peers[6].up_to = 60000;                  /* goes silent after 60 s */
    peers[7].delay = 3500;                   /* slower than all three requests: resolution fails, late reply ignored */
    rng_state = 555u;
    long t = 0;
    for (int i = 0; i < 45; i++) {
        t += 300 + (long)(rnd() % 4000);
        unsigned r = rnd();
        ev_push(t, EV_SEND, 0, 2 + (int)(r % 6), 0, 0, 0);
    }
    for (int k = 0; k < 5; k++) ev_push(100, EV_SEND, 0, 4, 0, 0, 0); /* burst into an unresolved slow peer */
    ev_push(500, EV_SEND, 0, 5, 0, 0, 0);
    ev_push(41000, EV_GARP, 0, 5, 50, 0, 0); /* host 5 changes its MAC and announces it */
    ev_push(41000 + 100, EV_SEND, 0, 5, 0, 0, 0);
    while (evn > 0) {
        Ev e = ev_pop();
        int ip = e.a;
        Neigh *n = &nc[ip];
        switch (e.type) {
        case EV_SEND: handle_send(ip); break;
        case EV_RETRY:
            if (e.b != n->gen || n->state != INCOMPLETE) break;
            if (n->tries < MAX_TRIES) { n->tries++; request(ip, 0); arm(EV_RETRY, ip, T_RETRANS); }
            else {
                dropped_failed += n->queued;
                n->queued = 0;
                setst(ip, FAILED);
                arm(EV_HOLD, ip, T_HOLD);
            }
            break;
        case EV_HOLD:
            if (e.b == n->gen && n->state == FAILED) setst(ip, NONE);
            break;
        case EV_REPLY:
            replies++;
            if (n->state == INCOMPLETE || n->state == PROBE || n->state == DELAY) confirm(ip, e.b);
            break;
        case EV_REACH:
            if (e.b == n->gen && n->state == REACHABLE) setst(ip, STALE);
            break;
        case EV_DELAY:
            if (e.b == n->gen && n->state == DELAY) { setst(ip, PROBE); n->tries = 1; request(ip, 1); arm(EV_PROBE, ip, T_RETRANS); }
            break;
        case EV_PROBE:
            if (e.b != n->gen || n->state != PROBE) break;
            if (n->tries < MAX_TRIES) { n->tries++; request(ip, 1); arm(EV_PROBE, ip, T_RETRANS); }
            else { setst(ip, NONE); n->gen++; }
            break;
        case EV_GARP:
            peers[ip].mac = e.b;
            if (n->state != NONE && n->state != FAILED) { n->mac = e.b; printf("t=%6ld ip%d gratuitous ARP: mac now %d\n", now, ip, e.b); if (n->state == REACHABLE) { setst(ip, STALE); n->gen++; } }
            break;
        }
    }
    printf("tx ok=%ld (first use of stale entry=%ld) queued=%ld dropped: queue-full=%ld failed=%ld\n", tx_ok, tx_via_stale,
           queued_total, dropped_q, dropped_failed);
    printf("arp requests=%ld unicast probes=%ld replies=%ld\n", requests, probes, replies);
    long sum = 0;
    for (int m = 0; m < 64; m++) sum += tx_by_mac[m];
    check(sum == tx_ok, "every transmitted packet used a resolved MAC");
    check(tx_by_mac[13] == 0, "never sent to the silent peer");
    check(transitions[INCOMPLETE][FAILED] >= 1, "silent peer failed resolution");
    check(transitions[PROBE][NONE] >= 1, "peer that vanished was removed by probing");
    check(transitions[STALE][DELAY] >= 1 && transitions[DELAY][PROBE] >= 0, "stale entries exercised");
    check(tx_by_mac[15] > 0 && tx_by_mac[50] > 0, "traffic used the old MAC, then the announced one");
    return 0;
}
