/*
 * title: Learning switch with MAC table aging and station moves
 * topic: networking
 * covers: MAC learning, flooding vs unicast forwarding, aging timer, table capacity eviction, host mobility, unknown-unicast counting
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

enum { NPORT = 6, NHOST = 12, MAXTAB = 16, BCAST = 0xff };

typedef struct { int mac, port; long last_seen; int used; } Entry;
typedef struct {
    Entry tab[MAXTAB];
    int cap;
    long age_limit;
    long learned, moved, aged, evicted;
    long unicast, flooded, filtered, bcast;
    long copies_out;
} Sw;

static int host_port[NHOST];      /* where each host is really plugged in */
static long delivered_to[NHOST];   /* frames the host accepted (addressed to it or broadcast) */
static long wasted[NHOST];         /* frames delivered to the wrong host by flooding */
static long misdelivered_stale;

static Entry *lookup(Sw *s, int mac) {
    for (int i = 0; i < s->cap; i++) if (s->tab[i].used && s->tab[i].mac == mac) return &s->tab[i];
    return NULL;
}
static void sweep(Sw *s, long t) {
    for (int i = 0; i < s->cap; i++)
        if (s->tab[i].used && t - s->tab[i].last_seen > s->age_limit) { s->tab[i].used = 0; s->aged++; }
}
static void learn(Sw *s, int mac, int port, long t) {
    Entry *e = lookup(s, mac);
    if (e) {
        if (e->port != port) { e->port = port; s->moved++; }
        e->last_seen = t;
        return;
    }
    int slot = -1;
    for (int i = 0; i < s->cap; i++) if (!s->tab[i].used) { slot = i; break; }
    if (slot < 0) { /* evict the least recently seen entry */
        slot = 0;
        for (int i = 1; i < s->cap; i++) if (s->tab[i].last_seen < s->tab[slot].last_seen) slot = i;
        s->evicted++;
    }
    s->tab[slot].used = 1; s->tab[slot].mac = mac; s->tab[slot].port = port; s->tab[slot].last_seen = t;
    s->learned++;
}
static void deliver_to_port(int port, int src, int dst) {
    for (int h = 0; h < NHOST; h++) {
        if (host_port[h] != port || h == src) continue;
        if (dst == BCAST || dst == h) delivered_to[h]++;
        else wasted[h]++;
    }
}
static void frame(Sw *s, long t, int src, int dst) {
    int in = host_port[src];
    sweep(s, t);
    learn(s, src, in, t);
    if (dst == BCAST) {
        s->bcast++;
        for (int p = 0; p < NPORT; p++) if (p != in) { deliver_to_port(p, src, dst); s->copies_out++; }
        return;
    }
    Entry *e = lookup(s, dst);
    if (!e) {
        s->flooded++;
        for (int p = 0; p < NPORT; p++) if (p != in) { deliver_to_port(p, src, dst); s->copies_out++; }
    } else if (e->port == in) {
        s->filtered++;
    } else {
        s->unicast++;
        s->copies_out++;
        if (host_port[dst] != e->port) misdelivered_stale++;
        deliver_to_port(e->port, src, dst);
    }
}

static void run(int cap, long age_limit) {
    Sw s;
    memset(&s, 0, sizeof s);
    s.cap = cap; s.age_limit = age_limit;
    memset(delivered_to, 0, sizeof delivered_to);
    memset(wasted, 0, sizeof wasted);
    misdelivered_stale = 0;
    for (int h = 0; h < NHOST; h++) host_port[h] = h % NPORT;
    rng_state = 0xC0FFEEu;
    long sent_unicast = 0, sent_bcast = 0;
    long t = 0;
    for (int i = 0; i < 400; i++) {
        t += 1 + (long)(rnd() % 20);
        if (i == 150) host_port[3] = 4; /* host 3 moves from port 3 to port 4 silently */
        if (i == 200) t += 700;          /* long idle period lets entries age out */
        unsigned r = rnd();
        unsigned q = rnd();
        int src = (int)(r % NHOST);
        int dst = (int)(q % (NHOST + 1));
        if (dst == NHOST) dst = BCAST;
        else if (dst == src) dst = (dst + 1) % NHOST;
        frame(&s, t, src, dst);
        if (dst == BCAST) sent_bcast++; else sent_unicast++;
    }
    long got = 0, waste = 0;
    for (int h = 0; h < NHOST; h++) { got += delivered_to[h]; waste += wasted[h]; }
    printf("cap=%2d age=%4ld: unicast=%3ld flooded=%3ld filtered=%3ld bcast=%3ld learned=%3ld moved=%ld aged=%3ld evicted=%3ld wasted_copies=%3ld stale_misdelivery=%ld\n",
           cap, age_limit, s.unicast, s.flooded, s.filtered, s.bcast, s.learned, s.moved, s.aged, s.evicted, waste,
           misdelivered_stale);
    check(s.unicast + s.flooded + s.filtered == sent_unicast, "every unicast frame classified");
    check(s.bcast == sent_bcast, "broadcasts counted");
    check(got >= (NHOST - 1) * sent_bcast, "every broadcast reaches every other host");
}

int main(void) {
    run(16, 300);
    run(16, 50);
    run(6, 300);
    run(4, 300);
    run(16, 100000);
    return 0;
}
