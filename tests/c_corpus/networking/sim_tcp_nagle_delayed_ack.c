/*
 * title: Nagle algorithm meets delayed ACK
 * topic: networking
 * covers: Nagle, delayed ACK timer, write-write-read stall, coalescing, discrete-event simulation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 1024
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

enum { EV_WRITE, EV_DATA, EV_ACK, EV_DACK };
enum { MSS = 100, OWD = 10, DACK_MS = 40 };

typedef struct {
    int nagle, delayed;
    int buf, outstanding, total_written;
    int segs, acks, small_segs;
    int rcv_total, rcv_pending_segs, dack_armed, dack_gen;
    long last_delivery, last_ack_at, first_delivery;
    int max_outstanding;
} Conn;
static Conn c;

static void try_send(void) {
    while (c.buf > 0 && (!c.nagle || c.outstanding == 0 || c.buf >= MSS)) {
        int len = c.buf < MSS ? c.buf : MSS;
        c.buf -= len;
        c.outstanding += len;
        if (c.outstanding > c.max_outstanding) c.max_outstanding = c.outstanding;
        c.segs++;
        if (len < MSS) c.small_segs++;
        ev_push(now + OWD, EV_DATA, 1, len, 0, 0, 0);
    }
}
static void send_ack(void) {
    c.acks++;
    c.rcv_pending_segs = 0;
    c.dack_armed = 0;
    ev_push(now + OWD, EV_ACK, 0, c.rcv_total, 0, 0, 0);
}

static void run(const char *name, int nagle, int delayed, int nwrites, const int *sizes, const long *times) {
    memset(&c, 0, sizeof c);
    c.nagle = nagle;
    c.delayed = delayed;
    evn = 0; now = 0;
    int total = 0;
    for (int i = 0; i < nwrites; i++) {
        ev_push(times[i], EV_WRITE, 0, sizes[i], 0, 0, 0);
        total += sizes[i];
    }
    int acked = 0;
    while (evn > 0) {
        Ev e = ev_pop();
        switch (e.type) {
        case EV_WRITE:
            c.buf += e.a;
            c.total_written += e.a;
            try_send();
            break;
        case EV_DATA:
            c.rcv_total += e.a;
            if (c.first_delivery == 0) c.first_delivery = now;
            c.last_delivery = now;
            c.rcv_pending_segs++;
            if (!c.delayed || c.rcv_pending_segs >= 2) send_ack();
            else if (!c.dack_armed) {
                c.dack_armed = 1;
                ev_push(now + DACK_MS, EV_DACK, 1, ++c.dack_gen, 0, 0, 0);
            }
            break;
        case EV_DACK:
            if (c.dack_armed && e.a == c.dack_gen) send_ack();
            break;
        case EV_ACK:
            c.outstanding -= e.a - acked;
            acked = e.a;
            c.last_ack_at = now;
            try_send();
            break;
        }
    }
    check(c.rcv_total == total, "all bytes delivered");
    check(acked == total && c.outstanding == 0 && c.buf == 0, "all bytes acked");
    printf("  %-22s nagle=%d delayed=%d  segs=%2d (small %2d) acks=%2d  last_data@%3ld  last_ack@%3ld\n", name, nagle,
           delayed, c.segs, c.small_segs, c.acks, c.last_delivery, c.last_ack_at);
}

int main(void) {
    static const int trickle_sz[20] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10};
    long trickle_t[20];
    for (int i = 0; i < 20; i++) trickle_t[i] = 5L * i;
    static const int req_sz[2] = {4, 60};
    static const long req_t[2] = {0, 1};
    static const int bulk_sz[6] = {250, 250, 250, 250, 250, 130};
    static const long bulk_t[6] = {0, 0, 0, 0, 0, 0};
    for (int n = 0; n < 2; n++) {
        for (int d = 0; d < 2; d++) {
            printf("config nagle=%d delayed_ack=%d\n", n, d);
            run("trickle 20x10B/5ms", n, d, 20, trickle_sz, trickle_t);
            run("header+body (4B,60B)", n, d, 2, req_sz, req_t);
            run("bulk 1130B burst", n, d, 6, bulk_sz, bulk_t);
        }
    }
    /* the classic stall: header then body with Nagle+delayed ACK waits for the delayed ACK timer */
    run("stall check", 1, 1, 2, req_sz, req_t);
    long stalled = c.last_delivery;
    run("stall check", 0, 1, 2, req_sz, req_t);
    long free_run = c.last_delivery;
    printf("body delivered at %ld with Nagle, %ld without: stall = %ld ms\n", stalled, free_run, stalled - free_run);
    check(stalled - free_run >= DACK_MS - 2 * OWD - 1, "Nagle plus delayed ACK stalls the body");
    return 0;
}
