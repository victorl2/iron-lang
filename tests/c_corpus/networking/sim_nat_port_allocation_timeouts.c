/*
 * title: NAT translation table with port pool and timeouts
 * topic: networking
 * covers: NAPT, port preservation, endpoint-independent vs symmetric mapping, inbound filtering, protocol timeouts, pool exhaustion
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

enum { MAXMAP = 128, PBASE = 2000, MAXC = 16 };
enum { CONE, RESTRICTED, SYMMETRIC };
static const char *modename[] = {"full-cone", "port-restricted", "symmetric"};
enum { UDP = 0, TCP = 1 };
static const long timeout_of[2] = {30, 300};

typedef struct { int ip, port; } Ep;
typedef struct {
    int used, proto;
    Ep in, ext;          /* ext only meaningful for symmetric mappings */
    int nat_port;
    long last;
    Ep contacts[MAXC];
    int nc;
} Map;

typedef struct {
    int mode, pool;
    Map m[MAXMAP];
    int cursor;
    long created, refreshed, expired, exhausted, in_ok, in_blocked, in_nomap, preserved;
    int active_peak;
} Nat;

static int port_free(Nat *n, int proto, int port) {
    for (int i = 0; i < MAXMAP; i++) if (n->m[i].used && n->m[i].proto == proto && n->m[i].nat_port == port) return 0;
    return 1;
}
static void expire(Nat *n, long t) {
    for (int i = 0; i < MAXMAP; i++)
        if (n->m[i].used && t - n->m[i].last > timeout_of[n->m[i].proto]) { n->m[i].used = 0; n->expired++; }
}
static int active(Nat *n) {
    int c = 0;
    for (int i = 0; i < MAXMAP; i++) c += n->m[i].used;
    return c;
}
static Map *find_out(Nat *n, int proto, Ep in, Ep ext) {
    for (int i = 0; i < MAXMAP; i++) {
        Map *m = &n->m[i];
        if (!m->used || m->proto != proto || m->in.ip != in.ip || m->in.port != in.port) continue;
        if (n->mode == SYMMETRIC && (m->ext.ip != ext.ip || m->ext.port != ext.port)) continue;
        return m;
    }
    return NULL;
}
static void note_contact(Map *m, Ep ext) {
    for (int i = 0; i < m->nc; i++) if (m->contacts[i].ip == ext.ip && m->contacts[i].port == ext.port) return;
    if (m->nc < MAXC) m->contacts[m->nc++] = ext;
}
static int outbound(Nat *n, long t, int proto, Ep in, Ep ext) {
    expire(n, t);
    Map *m = find_out(n, proto, in, ext);
    if (m) {
        m->last = t;
        note_contact(m, ext);
        n->refreshed++;
        return m->nat_port;
    }
    int slot = -1;
    for (int i = 0; i < MAXMAP; i++) if (!n->m[i].used) { slot = i; break; }
    int port = -1;
    /* port preservation: reuse the internal port number when it lies in the pool and is free */
    if (in.port >= PBASE && in.port < PBASE + n->pool && port_free(n, proto, in.port)) { port = in.port; n->preserved++; }
    else {
        for (int k = 0; k < n->pool; k++) {
            int p = PBASE + (n->cursor + k) % n->pool;
            if (port_free(n, proto, p)) { port = p; n->cursor = (p - PBASE + 1) % n->pool; break; }
        }
    }
    if (slot < 0 || port < 0) { n->exhausted++; return -1; }
    m = &n->m[slot];
    memset(m, 0, sizeof *m);
    m->used = 1; m->proto = proto; m->in = in; m->ext = ext; m->nat_port = port; m->last = t;
    note_contact(m, ext);
    n->created++;
    int a = active(n);
    if (a > n->active_peak) n->active_peak = a;
    return port;
}
/* returns 1 if delivered, writing the internal endpoint */
static int inbound(Nat *n, long t, int proto, Ep from, int nat_port, Ep *deliver) {
    expire(n, t);
    for (int i = 0; i < MAXMAP; i++) {
        Map *m = &n->m[i];
        if (!m->used || m->proto != proto || m->nat_port != nat_port) continue;
        int ok = 1;
        if (n->mode == RESTRICTED) {
            ok = 0;
            for (int c = 0; c < m->nc; c++) if (m->contacts[c].ip == from.ip && m->contacts[c].port == from.port) ok = 1;
        } else if (n->mode == SYMMETRIC) ok = m->ext.ip == from.ip && m->ext.port == from.port;
        if (!ok) { n->in_blocked++; return 0; }
        m->last = t;
        n->in_ok++;
        *deliver = m->in;
        return 1;
    }
    n->in_nomap++;
    return 0;
}

static void run(int mode, int pool) {
    static Nat nat;
    memset(&nat, 0, sizeof nat);
    nat.mode = mode; nat.pool = pool;
    rng_state = 0xBEEF5u;
    struct { Ep in, ext; int proto, nport; } seen[64];
    int nseen = 0;
    long t = 0, sent_replies = 0, delivered_ok = 0, stranger_ok = 0;
    for (int i = 0; i < 600; i++) {
        t += (long)(rnd() % 6);
        if (i == 300) t += 400;
        unsigned k = rnd();
        unsigned x = rnd();
        unsigned y = rnd();
        int kind = (int)(k % 10);
        if (kind < 6 || nseen == 0) {
            Ep in = {1 + (int)(x % 12), 2000 + (int)((x >> 8) % 40)};
            Ep ext = {100 + (int)(y % 5), (int)(y & 8) ? 443 : 53};
            int proto = ext.port == 53 ? UDP : TCP;
            int np = outbound(&nat, t, proto, in, ext);
            if (np >= 0 && nseen < 64) {
                seen[nseen].in = in; seen[nseen].ext = ext; seen[nseen].proto = proto; seen[nseen].nport = np; nseen++;
            }
        } else if (kind < 9) {
            int j = (int)(y % (unsigned)nseen);
            sent_replies++;
            Ep got;
            if (inbound(&nat, t, seen[j].proto, seen[j].ext, seen[j].nport, &got)) delivered_ok++;
        } else {
            Ep stranger = {200 + (int)(y % 3), 6000};
            Ep got;
            int np = PBASE + (int)(x % (unsigned)pool);
            if (inbound(&nat, t, TCP, stranger, np, &got)) { /* only full cone lets strangers in */
                check(mode == CONE, "strangers pass only through a full-cone NAT");
                stranger_ok++;
            }
        }
        /* invariant: no two live mappings share (proto, nat_port) */
        if (i % 50 == 0)
            for (int a = 0; a < MAXMAP; a++)
                for (int b = a + 1; b < MAXMAP; b++)
                    if (nat.m[a].used && nat.m[b].used && nat.m[a].proto == nat.m[b].proto)
                        check(nat.m[a].nat_port != nat.m[b].nat_port, "external ports unique per protocol");
    }
    printf("%-15s pool=%3d: created=%3ld refreshed=%3ld expired=%3ld preserved=%3ld exhausted=%3ld in_ok=%3ld/%3ld blocked=%3ld nomap=%3ld peak=%d\n",
           modename[mode], pool, nat.created, nat.refreshed, nat.expired, nat.preserved, nat.exhausted, nat.in_ok, sent_replies,
           nat.in_blocked, nat.in_nomap, nat.active_peak);
    check(delivered_ok + stranger_ok == nat.in_ok, "inbound accounting");
    check(nat.created == nat.expired + active(&nat), "every mapping either expired or is still live");
}

int main(void) {
    for (int m = 0; m < 3; m++) { run(m, 200); run(m, 24); }
    return 0;
}
