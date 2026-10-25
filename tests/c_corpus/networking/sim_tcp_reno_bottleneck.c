/*
 * title: TCP Reno over a drop-tail bottleneck
 * topic: networking
 * covers: slow start, congestion avoidance, fast retransmit, fast recovery, RTO, drop-tail queue, event simulation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 4096
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

enum { EV_ARRIVE, EV_DEPART, EV_DATA, EV_ACK, EV_RTO };
enum { NSEG = 1500, QCAP = 15, SERVICE = 10, PROP = 100, RTO_T = 800 };

static int una, nxt, cwnd = 1, ssthresh = 32, dup, in_fr, recover_pt, ca_acc, gen;
static int qlen, drops, rcv_nxt, marked[NSEG + 1];
static long busy_until;
static int fast_retx, timeouts, retx_total, sent_total, max_cwnd, ss_exit_cwnd = -1;
static int sent_once[NSEG + 1];
static long samples_t[64];
static int samples_w[64], samples_ss[64], nsamples;

static void send_pkt(int seq) {
    sent_total++;
    if (sent_once[seq]) retx_total++;
    sent_once[seq] = 1;
    ev_push(now + PROP, EV_ARRIVE, 0, seq, 0, 0, 0);
}
static void arm(void) { ev_push(now + RTO_T, EV_RTO, 0, ++gen, 0, 0, 0); }
static void pump(void) {
    while (nxt < NSEG && nxt < una + cwnd) {
        if (nxt == una) arm();
        send_pkt(nxt++);
    }
}
static void set_cwnd(int w) {
    cwnd = w;
    if (cwnd > max_cwnd) max_cwnd = cwnd;
}

int main(void) {
    long next_sample = 300;
    pump();
    while (evn > 0 && una < NSEG) {
        Ev e = ev_pop();
        if (now >= next_sample && nsamples < 60) {
            samples_t[nsamples] = now;
            samples_w[nsamples] = cwnd;
            samples_ss[nsamples] = cwnd < ssthresh ? 1 : 0;
            nsamples++;
            next_sample += 300;
        }
        switch (e.type) {
        case EV_ARRIVE:
            if (qlen >= QCAP) { drops++; break; }
            qlen++;
            if (busy_until < now) busy_until = now;
            busy_until += SERVICE;
            ev_push(busy_until, EV_DEPART, 0, 0, 0, 0, 0);
            ev_push(busy_until + PROP, EV_DATA, 0, e.a, 0, 0, 0);
            break;
        case EV_DEPART: qlen--; break;
        case EV_DATA:
            marked[e.a] = 1;
            while (rcv_nxt < NSEG && marked[rcv_nxt]) rcv_nxt++;
            ev_push(now + PROP, EV_ACK, 0, rcv_nxt, 0, 0, 0);
            break;
        case EV_ACK:
            if (e.a > una) {
                int newly = e.a - una;
                una = e.a;
                dup = 0;
                if (in_fr) {
                    if (una >= recover_pt) { in_fr = 0; set_cwnd(ssthresh); }
                    else { send_pkt(una); set_cwnd(cwnd > newly ? cwnd - newly + 1 : 1); }
                } else if (cwnd < ssthresh) {
                    set_cwnd(cwnd + newly);
                } else {
                    ca_acc += newly;
                    while (ca_acc >= cwnd) { ca_acc -= cwnd; set_cwnd(cwnd + 1); }
                }
                if (ss_exit_cwnd < 0 && cwnd >= ssthresh) ss_exit_cwnd = cwnd;
                gen++;
                if (una < nxt) arm();
                pump();
            } else if (e.a == una && una < nxt) {
                dup++;
                if (dup == 3 && !in_fr) {
                    int flight = nxt - una;
                    ssthresh = flight / 2 > 2 ? flight / 2 : 2;
                    send_pkt(una);
                    fast_retx++;
                    in_fr = 1;
                    recover_pt = nxt;
                    set_cwnd(ssthresh + 3);
                } else if (in_fr) {
                    set_cwnd(cwnd + 1);
                    pump();
                }
            }
            break;
        case EV_RTO:
            if (e.a != gen || una >= nxt) break;
            timeouts++;
            ssthresh = (nxt - una) / 2 > 2 ? (nxt - una) / 2 : 2;
            set_cwnd(1);
            cwnd = 1;
            in_fr = 0;
            dup = 0;
            ca_acc = 0;
            nxt = una + 1;
            send_pkt(una);
            arm();
            break;
        }
        check(cwnd >= 1, "cwnd positive");
    }
    check(una == NSEG && rcv_nxt == NSEG, "all delivered");
    printf("  time  cwnd phase\n");
    for (int i = 0; i < nsamples; i += 3)
        printf("%6ld %5d %s\n", samples_t[i], samples_w[i], samples_ss[i] ? "slow-start" : "avoidance/recovery");
    printf("finished at t=%ld\n", now);
    printf("segments=%d sent=%d retransmitted=%d queue drops=%d\n", NSEG, sent_total, retx_total, drops);
    printf("fast retransmits=%d timeouts=%d max cwnd=%d final ssthresh=%d\n", fast_retx, timeouts, max_cwnd, ssthresh);
    check(drops > 0, "bottleneck overflowed at least once");
    check(sent_total >= NSEG + fast_retx, "retransmissions counted");
    check(max_cwnd > QCAP, "window grew past the queue size");
    return 0;
}
