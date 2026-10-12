/*
 * title: DHCP server lease allocation state machine
 * topic: networking
 * covers: DORA exchange, offer hold, lease expiry and reuse, renewal at T1, sticky MAC bindings, NAK, DECLINE quarantine, pool exhaustion
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

enum { POOL = 5, BASE_HOST = 100, LEASE = 100, OFFER_HOLD = 10, QUARANTINE = 60, NCLIENT = 8 };
enum { FREE, OFFERED, BOUND, BAD };
enum { EV_START, EV_REQ, EV_RENEW, EV_RELEASE, EV_REBOOT, EV_VANISH };

typedef struct { int state, mac; long expiry, last_used; } Lease;
typedef struct { int ip, gen, alive, slow, decline_once; } Client;
static Lease lease[POOL];
static Client cl[NCLIENT];
static long n_offer, n_ack, n_nak, n_noaddr, n_renew, n_expire, n_release, n_decline, n_sticky, n_reuse;

static int mac_of(int c) { return 0xa0 + c; }

static void expire_all(void) {
    for (int i = 0; i < POOL; i++) {
        Lease *l = &lease[i];
        if (l->state == FREE || l->expiry > now) continue;
        if (l->state == BOUND) { n_expire++; printf("  t=%3ld lease .%d of client %d expired\n", now, BASE_HOST + i, l->mac - 0xa0); }
        l->state = FREE;
        l->last_used = l->expiry;
    }
}
static void audit(void) {
    int seen[NCLIENT] = {0};
    for (int i = 0; i < POOL; i++)
        if (lease[i].state == BOUND) {
            int c = lease[i].mac - 0xa0;
            check(c >= 0 && c < NCLIENT, "bound MAC is a known client");
            check(!seen[c], "a client never holds two leases");
            seen[c] = 1;
        }
    for (int c = 0; c < NCLIENT; c++)
        if (cl[c].ip >= 0 && cl[c].alive && lease[cl[c].ip].state == BOUND && lease[cl[c].ip].mac != mac_of(c))
            fail("client believes it owns an address bound to someone else");
}

static int pick_address(int mac, int hint) {
    for (int i = 0; i < POOL; i++)
        if ((lease[i].state == OFFERED || lease[i].state == BOUND) && lease[i].mac == mac) return i;
    /* sticky: the address this MAC used last, if still unused */
    for (int i = 0; i < POOL; i++)
        if (lease[i].state == FREE && lease[i].mac == mac && lease[i].last_used > 0) { n_sticky++; return i; }
    if (hint >= 0 && lease[hint].state == FREE) return hint;
    int best = -1; /* least recently used free address; never-used ones (last_used 0) come first */
    for (int i = 0; i < POOL; i++)
        if (lease[i].state == FREE && (best < 0 || lease[i].last_used < lease[best].last_used)) best = i;
    return best;
}

static void arm_renew(int c) { if (now < 300) ev_push(now + LEASE / 2, EV_RENEW, c, cl[c].gen, 0, 0, 0); }

static void do_discover(int c) {
    int mac = mac_of(c);
    int i = pick_address(mac, -1);
    if (i < 0) {
        n_noaddr++;
        printf("  t=%3ld client %d DISCOVER: no free address, retry in 40\n", now, c);
        if (now < 300) ev_push(now + 40, EV_START, c, 0, 0, 0, 0);
        return;
    }
    if (lease[i].mac != mac && lease[i].last_used > 0) n_reuse++;
    lease[i].state = OFFERED; lease[i].mac = mac; lease[i].expiry = now + OFFER_HOLD;
    n_offer++;
    printf("  t=%3ld client %d DISCOVER: offer .%d\n", now, c, BASE_HOST + i);
    if (cl[c].decline_once) {
        cl[c].decline_once = 0;
        lease[i].state = BAD; lease[i].expiry = now + QUARANTINE; lease[i].mac = 0;
        n_decline++;
        printf("  t=%3ld client %d DECLINE .%d (address conflict), quarantined until %ld\n", now, c, BASE_HOST + i, now + QUARANTINE);
        ev_push(now + 2, EV_START, c, 0, 0, 0, 0);
        return;
    }
    ev_push(now + (cl[c].slow ? 15 : 1), EV_REQ, c, i, 0, 0, 0);
}
static void do_request(int c, int ip, int kind) {
    int mac = mac_of(c);
    Lease *l = &lease[ip];
    int ok = (l->state == OFFERED || l->state == BOUND) && l->mac == mac;
    if (kind == 1 && l->state == FREE && l->mac == mac) ok = 1; /* INIT-REBOOT on a lapsed but unclaimed address */
    if (!ok) {
        n_nak++;
        printf("  t=%3ld client %d REQUEST .%d: NAK, restarting discovery\n", now, c, BASE_HOST + ip);
        cl[c].ip = -1; cl[c].slow = 0;
        ev_push(now + 1, EV_START, c, 0, 0, 0, 0);
        return;
    }
    l->state = BOUND; l->mac = mac; l->expiry = now + LEASE;
    n_ack++;
    cl[c].ip = ip; cl[c].gen++;
    if (kind != 2) printf("  t=%3ld client %d REQUEST .%d: ACK, lease until %ld\n", now, c, BASE_HOST + ip, now + LEASE);
    arm_renew(c);
}

int main(void) {
    for (int c = 0; c < NCLIENT; c++) { cl[c].ip = -1; cl[c].alive = 1; }
    cl[6].slow = 1;
    cl[3].decline_once = 1;
    for (int c = 0; c < NCLIENT; c++) ev_push(c * 4, EV_START, c, 0, 0, 0, 0);
    ev_push(90, EV_RELEASE, 1, 0, 0, 0, 0);
    ev_push(60, EV_VANISH, 2, 0, 0, 0, 0);
    ev_push(130, EV_REBOOT, 0, 0, 0, 0, 0);
    ev_push(180, EV_REBOOT, 2, 0, 0, 0, 0);
    ev_push(150, EV_RELEASE, 4, 0, 0, 0, 0);
    ev_push(260, EV_START, 1, 0, 0, 0, 0);
    while (evn > 0) {
        Ev e = ev_pop();
        int c = e.node;
        if (now > 300) break; /* simulation horizon */
        expire_all();
        switch (e.type) {
        case EV_START: cl[c].alive = 1; if (cl[c].ip < 0 || lease[cl[c].ip].mac != mac_of(c) || lease[cl[c].ip].state != BOUND) do_discover(c); break;
        case EV_REQ: do_request(c, e.a, 0); break;
        case EV_RENEW:
            if (!cl[c].alive || e.a != cl[c].gen || cl[c].ip < 0) break;
            n_renew++;
            do_request(c, cl[c].ip, 2);
            break;
        case EV_RELEASE:
            if (cl[c].ip >= 0 && lease[cl[c].ip].state == BOUND && lease[cl[c].ip].mac == mac_of(c)) {
                lease[cl[c].ip].state = FREE; lease[cl[c].ip].last_used = now;
                printf("  t=%3ld client %d RELEASE .%d\n", now, c, BASE_HOST + cl[c].ip);
                n_release++;
                cl[c].ip = -1; cl[c].gen++;
                cl[c].alive = 0;
            }
            break;
        case EV_VANISH:
            printf("  t=%3ld client %d vanishes without releasing\n", now, c);
            cl[c].alive = 0;
            break;
        case EV_REBOOT:
            if (cl[c].ip >= 0) {
                cl[c].alive = 1;
                printf("  t=%3ld client %d reboots, INIT-REBOOT for .%d\n", now, c, BASE_HOST + cl[c].ip);
                do_request(c, cl[c].ip, 1);
            }
            break;
        }
        audit();
    }
    printf("offers=%ld acks=%ld naks=%ld noaddr=%ld renewals=%ld expired=%ld released=%ld declined=%ld sticky=%ld reused=%ld\n",
           n_offer, n_ack, n_nak, n_noaddr, n_renew, n_expire, n_release, n_decline, n_sticky, n_reuse);
    check(n_nak >= 1 && n_decline == 1 && n_release >= 1 && n_expire >= 1, "scenario exercised NAK, decline, release, expiry");
    return 0;
}
