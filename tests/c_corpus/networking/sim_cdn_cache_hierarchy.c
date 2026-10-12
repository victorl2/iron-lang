/*
 * title: CDN cache hierarchy with Zipf traffic, TTL and revalidation
 * topic: networking
 * covers: edge and regional LRU caches, Zipf popularity, TTL expiry, conditional revalidation (304), origin offload, staleness, latency accounting
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

enum { NOBJ = 400, NEDGE = 4, NREG = 2, MAXCAP = 200, REQS = 40000 };
enum { LAT_EDGE = 5, LAT_REG = 25, LAT_ORIGIN = 120 };

typedef struct { int obj, version; long fetched, last_used; } Ent;
typedef struct { Ent e[MAXCAP]; int n, cap; long hits, misses, reval_304, reval_new; } Cache;

static int origin_version[NOBJ];
static long cum[NOBJ + 1];
static long origin_fetches, origin_304;

static Ent *find(Cache *c, int obj) {
    for (int i = 0; i < c->n; i++) if (c->e[i].obj == obj) return &c->e[i];
    return NULL;
}
static void insert(Cache *c, int obj, int version, long t) {
    if (c->cap == 0) return;
    Ent *e = find(c, obj);
    if (!e) {
        if (c->n < c->cap) e = &c->e[c->n++];
        else { /* evict least recently used */
            e = &c->e[0];
            for (int i = 1; i < c->n; i++) if (c->e[i].last_used < e->last_used) e = &c->e[i];
        }
    }
    e->obj = obj; e->version = version; e->fetched = t; e->last_used = t;
}

static int zipf_pick(unsigned r) {
    long x = (long)(r % (unsigned long)cum[NOBJ]);
    int lo = 0, hi = NOBJ - 1;
    while (lo < hi) { int m = (lo + hi) / 2; if (cum[m + 1] > x) hi = m; else lo = m + 1; }
    return lo;
}

/* fetch from origin, with an optional conditional version */
static int origin_get(int obj, int have_version, long *lat) {
    *lat += LAT_ORIGIN;
    if (have_version == origin_version[obj]) { origin_304++; return -1; }
    origin_fetches++;
    return origin_version[obj];
}

/* parent tier lookup (regional). Returns version delivered and adds latency. */
static int regional_get(Cache *r, int obj, int have_version, long t, long ttl, long *lat) {
    *lat += LAT_REG;
    Ent *e = r->cap ? find(r, obj) : NULL;
    if (e && t - e->fetched < ttl) { r->hits++; e->last_used = t; return e->version; }
    if (e) { /* stale: revalidate with the origin */
        int v = origin_get(obj, e->version, lat);
        if (v < 0) { r->reval_304++; e->fetched = t; e->last_used = t; return e->version; }
        r->reval_new++; e->version = v; e->fetched = t; e->last_used = t;
        return v;
    }
    r->misses++;
    int v = origin_get(obj, -1, lat);
    insert(r, obj, v, t);
    return v;
}

static long run(int edge_cap, int reg_cap, long ttl) {
    static Cache edge[NEDGE], reg[NREG];
    memset(edge, 0, sizeof edge); memset(reg, 0, sizeof reg);
    for (int i = 0; i < NEDGE; i++) edge[i].cap = edge_cap;
    for (int i = 0; i < NREG; i++) reg[i].cap = reg_cap;
    for (int i = 0; i < NOBJ; i++) origin_version[i] = 1;
    origin_fetches = origin_304 = 0;
    rng_state = 0xCD11u;
    long lat_total = 0, stale = 0;
    for (long t = 1; t <= REQS; t++) {
        if (t % 50 == 0) { unsigned r = rnd(); origin_version[zipf_pick(r)]++; } /* popular objects change more often */
        unsigned r1 = rnd(), r2 = rnd();
        int obj = zipf_pick(r1);
        int ei = (int)(r2 % NEDGE);
        Cache *ec = &edge[ei], *rc = &reg[ei / 2];
        long lat = LAT_EDGE;
        Ent *e = ec->cap ? find(ec, obj) : NULL;
        int served;
        if (e && t - e->fetched < ttl) { ec->hits++; e->last_used = t; served = e->version; }
        else if (e) { /* stale entry: ask the regional tier to revalidate */
            int v = regional_get(rc, obj, e->version, t, ttl, &lat);
            if (v == e->version) ec->reval_304++; else ec->reval_new++;
            e->version = v; e->fetched = t; e->last_used = t;
            served = v;
        } else {
            ec->misses++;
            served = regional_get(rc, obj, -1, t, ttl, &lat);
            insert(ec, obj, served, t);
        }
        if (served != origin_version[obj]) stale++;
        lat_total += lat;
    }
    long eh = 0, em = 0, e304 = 0, rh = 0, rm = 0, r304 = 0;
    for (int i = 0; i < NEDGE; i++) { eh += edge[i].hits; em += edge[i].misses; e304 += edge[i].reval_304; }
    for (int i = 0; i < NREG; i++) { rh += reg[i].hits; rm += reg[i].misses; r304 += reg[i].reval_304; }
    long full = origin_fetches;
    printf("edge=%3d regional=%3d ttl=%4ld | edge hit %2ld%% | regional hit %2ld%% (of %ld) | origin bodies=%5ld 304s=%5ld | mean latency %3ld.%02ld ms | stale served %ld.%02ld%%\n",
           edge_cap, reg_cap, ttl, eh * 100 / REQS, rh + rm + r304 ? rh * 100 / (rh + rm + r304) : 0L, rh + rm + r304, full, origin_304,
           lat_total / REQS, lat_total * 100 / REQS % 100, stale * 100 / REQS, stale * 10000 / REQS % 100);
    check(eh + em + e304 + edge[0].reval_new + edge[1].reval_new + edge[2].reval_new + edge[3].reval_new == REQS, "every request classified at the edge");
    return origin_fetches;
}

int main(void) {
    for (int i = 0; i < NOBJ; i++) cum[i + 1] = cum[i] + 1000000 / (i + 1);
    long ttls[] = {50, 500, 5000};
    for (int k = 0; k < 3; k++) {
        printf("TTL %ld\n", ttls[k]);
        long none = run(0, 0, ttls[k]);
        long flat = run(20, 0, ttls[k]);
        long tiered = run(20, 100, ttls[k]);
        run(60, 100, ttls[k]);
        long big = run(60, 200, ttls[k]);
        check(none == REQS, "without caches every request reaches the origin");
        check(flat < none && tiered < flat && big < tiered, "each added cache layer cuts origin body transfers");
    }
    return 0;
}
