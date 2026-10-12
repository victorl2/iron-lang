/*
 * title: Alternating-bit stop-and-wait ARQ with corruption
 * topic: networking
 * covers: alternating bit protocol, 16-bit checksum, frame corruption and loss, retransmission timer, duplicate suppression
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVCAP 512
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

enum { EV_FRAME, EV_TIMEOUT, EV_START };
enum { NMSG = 40, LEN = 8, DELAY = 10 };

typedef struct {
    int kind, bit;
    unsigned char data[LEN];
    unsigned csum;
} Frame;

static Frame frames[4096];
static int nframes;
static int loss_pct, corrupt_pct, timeout_len;

static unsigned checksum(const Frame *f) {
    unsigned sum = (unsigned)f->kind * 256u + (unsigned)f->bit;
    for (int i = 0; i < LEN; i += 2) sum += (unsigned)f->data[i] * 256u + f->data[i + 1];
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return ~sum & 0xffffu;
}

static long data_tx, ack_tx, lost, corrupted, discarded;

static void channel(const Frame *src, int dst) {
    unsigned r1 = rnd(), r2 = rnd(), r3 = rnd();
    if (src->kind == 0) data_tx++; else ack_tx++;
    if ((int)(r1 % 100) < loss_pct) { lost++; return; }
    if (nframes >= 4096) fail("frame store full");
    Frame *f = &frames[nframes];
    *f = *src;
    if ((int)(r2 % 100) < corrupt_pct) {
        corrupted++;
        f->data[r3 % LEN] ^= (unsigned char)(1u << ((r3 >> 8) % 8));
    }
    ev_push(now + DELAY, EV_FRAME, dst, nframes++, 0, 0, 0);
}

static unsigned char payloads[NMSG][LEN];

static void run(int loss, int corrupt, int tmo) {
    loss_pct = loss; corrupt_pct = corrupt; timeout_len = tmo;
    rng_state = 777u + (unsigned)(loss * 100 + corrupt * 7 + tmo);
    for (int m = 0; m < NMSG; m++)
        for (int i = 0; i < LEN; i++) payloads[m][i] = (unsigned char)(rnd() >> 11);
    evn = 0; now = 0; nframes = 0;
    data_tx = ack_tx = lost = corrupted = discarded = 0;
    int s_bit = 0, s_msg = 0, timer_gen = 0;
    int r_expect = 0, delivered = 0, dups = 0;
    unsigned char got[NMSG][LEN];
    long finish = 0;
    ev_push(0, EV_START, 0, 0, 0, 0, 0);
    while (evn > 0) {
        Ev e = ev_pop();
        if (e.type == EV_START || (e.type == EV_TIMEOUT && e.a == timer_gen)) {
            Frame f;
            memset(&f, 0, sizeof f);
            f.kind = 0; f.bit = s_bit;
            memcpy(f.data, payloads[s_msg], LEN);
            f.csum = checksum(&f);
            channel(&f, 1);
            ev_push(now + timeout_len, EV_TIMEOUT, 0, ++timer_gen, 0, 0, 0);
        } else if (e.type == EV_FRAME) {
            Frame *f = &frames[e.a];
            /* corruption changed the data but not the transmitted checksum */
            if (checksum(f) != f->csum) { discarded++; continue; }
            if (e.node == 1) { /* receiver */
                if (f->bit == r_expect) {
                    memcpy(got[delivered++], f->data, LEN);
                    r_expect ^= 1;
                } else dups++;
                Frame a;
                memset(&a, 0, sizeof a);
                a.kind = 1; a.bit = r_expect ^ 1; /* ack the last accepted bit */
                a.csum = checksum(&a);
                channel(&a, 0);
            } else { /* sender */
                if (f->bit == s_bit) {
                    timer_gen++;
                    s_bit ^= 1;
                    s_msg++;
                    if (s_msg == NMSG) { finish = now; break; }
                    ev_push(now, EV_START, 0, 0, 0, 0, 0); /* the START event sends and arms a fresh timer */
                }
            }
        }
    }
    check(delivered == NMSG, "all messages delivered");
    for (int m = 0; m < NMSG; m++) check(memcmp(got[m], payloads[m], LEN) == 0, "payload intact and in order");
    printf("loss=%2d%% corrupt=%2d%% tmo=%2d: finish@%5ld data_tx=%3ld ack_tx=%3ld lost=%3ld corrupt=%2ld bad_csum=%2ld dups=%3d\n",
           loss, corrupt, tmo, finish, data_tx, ack_tx, lost, corrupted, discarded, dups);
}

int main(void) {
    run(0, 0, 30);
    run(10, 0, 30);
    run(0, 15, 30);
    run(20, 20, 30);
    run(40, 10, 30);
    run(0, 0, 8); /* timer shorter than the round trip */
    run(10, 10, 8);
    return 0;
}
