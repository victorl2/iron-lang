/*
 * title: Mark-sweep collector versus reference counting on a mutating object graph
 * topic: data_structures
 * covers: object heap with index handles, explicit mark stack, sweep and free list, finalizers, reference counts, cyclic garbage leaks, reachability oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 1618033u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define NREF 3
#define MAXL 4000            /* logical objects over the whole run */
#define MSCAP 220
#define RCCAP 4000
#define MAXROOTS 12

typedef struct { int used, logical, marked, rc; int ref[NREF]; } Slot;

/* ---- the reference model: logical object graph plus root list ---- */
static int lref[MAXL][NREF];
static int nlogical;
static int roots[MAXROOTS], nroots;

static int pick_reachable(void) {
    int cur = roots[rnd() % (unsigned)nroots];
    int steps = (int)(rnd() % 4);
    while (steps--) {
        int k = (int)(rnd() % NREF);
        if (lref[cur][k] < 0) break;
        cur = lref[cur][k];
    }
    return cur;
}
static void logical_reachable(unsigned char *seen) {
    memset(seen, 0, (size_t)nlogical);
    int queue[MAXL], head = 0, tail = 0;
    for (int i = 0; i < nroots; i++) if (!seen[roots[i]]) { seen[roots[i]] = 1; queue[tail++] = roots[i]; }
    while (head < tail) {
        int o = queue[head++];
        for (int k = 0; k < NREF; k++) {
            int t = lref[o][k];
            if (t >= 0 && !seen[t]) { seen[t] = 1; queue[tail++] = t; }
        }
    }
}

/* ---- mark-sweep heap ---- */
static Slot ms[MSCAP];
static int ms_slot_of[MAXL];
static int ms_free, ms_live;
static long ms_finalized, ms_collections, ms_peak;

static void ms_init(void) {
    for (int i = 0; i < MSCAP; i++) { ms[i].used = 0; ms[i].ref[0] = i + 1; }
    ms[MSCAP - 1].ref[0] = -1;
    ms_free = 0;
    for (int i = 0; i < MAXL; i++) ms_slot_of[i] = -1;
}
static void ms_collect(void) {
    int stack[MSCAP], sp = 0;
    for (int i = 0; i < MSCAP; i++) ms[i].marked = 0;
    for (int i = 0; i < nroots; i++) {
        int s = ms_slot_of[roots[i]];
        CHECK(s >= 0);
        if (!ms[s].marked) { ms[s].marked = 1; stack[sp++] = s; }
    }
    while (sp) {
        int s = stack[--sp];
        for (int k = 0; k < NREF; k++) {
            int t = ms[s].ref[k];
            if (t >= 0 && !ms[t].marked) { ms[t].marked = 1; stack[sp++] = t; }
        }
    }
    for (int i = 0; i < MSCAP; i++) {
        if (ms[i].used && !ms[i].marked) {
            ms_slot_of[ms[i].logical] = -1;   /* finalizer */
            ms[i].used = 0;
            ms[i].ref[0] = ms_free;
            ms_free = i;
            ms_live--;
            ms_finalized++;
        }
    }
    ms_collections++;
}
static int ms_alloc(int logical) {
    if (ms_free < 0) ms_collect();
    CHECK(ms_free >= 0);
    int s = ms_free;
    ms_free = ms[s].ref[0];
    ms[s].used = 1; ms[s].logical = logical; ms[s].marked = 0;
    for (int k = 0; k < NREF; k++) ms[s].ref[k] = -1;
    ms_slot_of[logical] = s;
    ms_live++;
    if (ms_live > ms_peak) ms_peak = ms_live;
    return s;
}

/* ---- reference-counting heap ---- */
static Slot rc[RCCAP];
static int rc_slot_of[MAXL];
static int rc_next, rc_live;
static long rc_freed;

static void rc_dec(int s) {
    int stack[RCCAP], sp = 0;
    stack[sp++] = s;
    while (sp) {
        int x = stack[--sp];
        if (--rc[x].rc > 0) continue;
        rc[x].used = 0;
        rc_live--;
        rc_freed++;
        for (int k = 0; k < NREF; k++) if (rc[x].ref[k] >= 0) stack[sp++] = rc[x].ref[k];
    }
}
static int rc_alloc(int logical) {
    CHECK(rc_next < RCCAP);
    int s = rc_next++;
    rc[s].used = 1; rc[s].logical = logical; rc[s].rc = 0;
    for (int k = 0; k < NREF; k++) rc[s].ref[k] = -1;
    rc_slot_of[logical] = s;
    rc_live++;
    return s;
}
static void rc_set(int from, int k, int to) {
    if (to >= 0) rc[to].rc++;
    int old = rc[from].ref[k];
    rc[from].ref[k] = to;
    if (old >= 0) rc_dec(old);
}

/* ---- mutator operations applied to model and both heaps ---- */
static void set_ref(int from, int k, int to) {
    lref[from][k] = to;
    ms[ms_slot_of[from]].ref[k] = to < 0 ? -1 : ms_slot_of[to];
    rc_set(rc_slot_of[from], k, to < 0 ? -1 : rc_slot_of[to]);
}
static int new_object(void) {
    int l = nlogical++;
    CHECK(l < MAXL);
    for (int k = 0; k < NREF; k++) lref[l][k] = -1;
    ms_alloc(l);
    rc_alloc(l);
    return l;
}
static void verify_ms_matches_reachability(void) {
    static unsigned char seen[MAXL];
    logical_reachable(seen);
    int live = 0;
    for (int l = 0; l < nlogical; l++) {
        if (seen[l]) { CHECK(ms_slot_of[l] >= 0 && ms[ms_slot_of[l]].logical == l); live++; }
    }
    /* right after a collection, nothing unreachable may remain */
    int slots = 0;
    for (int i = 0; i < MSCAP; i++) if (ms[i].used) slots++;
    CHECK(slots == ms_live);
    CHECK(live == ms_live);
}
static void verify_rc_counts(void) {
    static int expect[RCCAP];
    memset(expect, 0, (size_t)rc_next * sizeof(int));
    for (int i = 0; i < nroots; i++) expect[rc_slot_of[roots[i]]]++;
    for (int s = 0; s < rc_next; s++)
        if (rc[s].used)
            for (int k = 0; k < NREF; k++) if (rc[s].ref[k] >= 0) expect[rc[s].ref[k]]++;
    for (int s = 0; s < rc_next; s++) if (rc[s].used) CHECK(rc[s].rc == expect[s]);
}

int main(void) {
    ms_init();
    long ops = 0, gcs_checked = 0;
    for (int step = 0; step < 3500 && nlogical < MAXL - 4; step++) {
        unsigned op = rnd() % 20;
        if (nroots == 0 || (op < 2 && nroots < MAXROOTS)) {
            int l = new_object();
            rc[rc_slot_of[l]].rc++;               /* root pin */
            roots[nroots++] = l;
        } else if (op < 8) {
            int p = pick_reachable();
            int k = (int)(rnd() % NREF);
            int c = new_object();
            set_ref(p, k, c);
        } else if (op < 13) {
            int a = pick_reachable(), b = pick_reachable();
            set_ref(a, (int)(rnd() % NREF), b);
        } else if (op < 17) {
            int a = pick_reachable();
            set_ref(a, (int)(rnd() % NREF), -1);
        } else if (op < 18 && nroots > 2) {
            int i = (int)(rnd() % (unsigned)nroots);
            rc_dec(rc_slot_of[roots[i]]);
            roots[i] = roots[--nroots];
        } else if (op < 19 && (rnd() % 8) == 0) {
            ms_collect();
            verify_ms_matches_reachability();
            gcs_checked++;
        }
        ops++;
        if (step % 250 == 0) verify_rc_counts();
    }
    ms_collect();
    verify_ms_matches_reachability();
    gcs_checked++;
    verify_rc_counts();
    static unsigned char seen[MAXL];
    logical_reachable(seen);
    int reachable = 0;
    for (int l = 0; l < nlogical; l++) reachable += seen[l];
    int leaked = 0, leaked_in_cycle = 0;
    for (int l = 0; l < nlogical; l++) {
        int s = rc_slot_of[l];
        if (rc[s].used) {
            if (seen[l]) continue;
            leaked++;
            /* a leaked object is kept alive only by other garbage; check it really has a referrer among leaked ones */
            int held = 0;
            for (int t = 0; t < nlogical; t++)
                if (!seen[t] && rc[rc_slot_of[t]].used)
                    for (int k = 0; k < NREF; k++) if (lref[t][k] == l) held = 1;
            CHECK(held);
            /* on a cycle: reachable from itself through leaked objects */
            unsigned char vis[MAXL];
            memset(vis, 0, (size_t)nlogical);
            int q[MAXL], h = 0, tl = 0, cyc = 0;
            for (int k = 0; k < NREF; k++) if (lref[l][k] >= 0 && !vis[lref[l][k]]) { vis[lref[l][k]] = 1; q[tl++] = lref[l][k]; }
            while (h < tl) {
                int o = q[h++];
                if (o == l) { cyc = 1; break; }
                for (int k = 0; k < NREF; k++) if (lref[o][k] >= 0 && !vis[lref[o][k]]) { vis[lref[o][k]] = 1; q[tl++] = lref[o][k]; }
            }
            leaked_in_cycle += cyc;
        }
    }
    CHECK(rc_live == reachable + leaked);
    CHECK(ms_live == reachable);
    printf("mutator ops=%ld logical objects=%d roots=%d reachable at end=%d\n", ops, nlogical, nroots, reachable);
    printf("mark-sweep: collections=%ld finalized=%ld peak live=%ld live now=%d (heap capacity %d)\n", ms_collections, ms_finalized, ms_peak, ms_live, MSCAP);
    printf("refcount: freed=%ld live=%d leaked garbage=%d (of which on cycles %d)\n", rc_freed, rc_live, leaked, leaked_in_cycle);
    printf("collections cross-checked against reachability: %ld\n", gcs_checked);
    CHECK(leaked > 0);
    return 0;
}
