/*
 * title: Ring-buffer FIFO allocator with wrap padding and deferred reclaim
 * topic: memory
 * covers: ring allocator, head and tail cursors, wrap padding block, out-of-order completion with lazy head advance, used-bytes accounting, full versus empty
 * deps: libc
 */
#define SEED 0x21A6F1F0ULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}
static void pat_fill(void *vp, size_t n, unsigned tag) {
    unsigned char *p = vp;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u);
}
static int pat_ok(const void *vp, size_t n, unsigned tag) {
    const unsigned char *p = vp;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u)) return 0;
    return 1;
}

#define CAP 2048u
#define HDR 8u
enum { S_LIVE = 1, S_DONE = 2, S_PAD = 3 };

static _Alignas(16) unsigned char ring[CAP];
static unsigned head, tail, used;
static unsigned long wraps, pad_bytes, ooo_frees, reclaimed, allocs, refused, peak_used;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, ring + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(ring + o, &v, 4); }
static unsigned total_of(unsigned o) { return HDR + rd(o); }   /* header stores payload size (multiple of 8) */

static void place(unsigned at, unsigned payload, unsigned state) {
    wr(at, payload); wr(at + 4, state);
}

static long r_alloc(size_t n) {
    unsigned payload = (unsigned)((n + 7) & ~(size_t)7);
    unsigned total = HDR + payload;
    if (total > CAP) { refused++; return -1; }
    if (used == 0) { head = tail = 0; }
    if (used == CAP) { refused++; return -1; }
    unsigned at;
    if (used == 0 || tail > head) {
        if (CAP - tail >= total) at = tail;
        else {
            /* not enough room before the end: pad out the tail and continue at offset 0 */
            if (total > head) { refused++; return -1; }
            unsigned padtotal = CAP - tail;
            place(tail, padtotal - HDR, S_PAD);
            used += padtotal; pad_bytes += padtotal; wraps++;
            at = 0;
        }
    } else {
        if (head - tail < total) { refused++; return -1; }
        at = tail;
    }
    place(at, payload, S_LIVE);
    used += total;
    tail = (at + total) % CAP;
    allocs++;
    if (used > peak_used) peak_used = used;
    return (long)(at + HDR);
}

static void reclaim(void) {
    while (used > 0) {
        unsigned st = rd(head + 4);
        if (st != S_DONE && st != S_PAD) break;
        unsigned t = total_of(head);
        wr(head + 4, 0);
        head = (head + t) % CAP;
        used -= t;
        reclaimed++;
    }
}

static void r_free(unsigned payload_off) {
    unsigned at = payload_off - HDR;
    CHECK(rd(at + 4) == S_LIVE);
    wr(at + 4, S_DONE);
    if (at != head) ooo_frees++;
    reclaim();
}

/* walk from head to tail and check the chain of blocks tiles exactly `used` bytes */
static unsigned ring_check(unsigned *nlive_blocks) {
    unsigned p = head, seen = 0, live = 0;
    while (seen < used) {
        unsigned st = rd(p + 4), t = total_of(p);
        CHECK((st == S_LIVE || st == S_DONE || st == S_PAD) && t >= HDR && t % 8 == 0);
        if (st == S_LIVE) live++;
        seen += t;
        p = (p + t) % CAP;
    }
    CHECK(seen == used);
    if (used > 0 && used < CAP) CHECK(p == tail);
    *nlive_blocks = live;
    return seen;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    static Rec q[400];
    int qh = 0, qt = 0;     /* live records kept in allocation order (circular index) */
    int count = 0;
    unsigned tag = 1, nlb;
    for (int step = 0; step < 25000; step++) {
        unsigned pa = (step / 4000) % 2 == 0 ? 55 : 40;
        if (count == 0 || (rnd() % 100 < pa && count < 390)) {
            size_t n = 1 + rnd() % 180;
            if (rnd() % 30 == 0) n = 400 + rnd() % 500;
            long off = r_alloc(n);
            if (off < 0) {
                /* when full, consumers catch up */
                if (count > 0) {
                    Rec r = q[qh]; qh = (qh + 1) % 400; count--;
                    CHECK(pat_ok(ring + r.off, r.n, r.tag));
                    r_free(r.off);
                }
                continue;
            }
            pat_fill(ring + off, n, tag);
            q[qt].off = (unsigned)off; q[qt].n = n; q[qt].tag = tag++;
            qt = (qt + 1) % 400; count++;
        } else {
            /* mostly FIFO completion, sometimes an arbitrary in-flight block finishes first */
            int idx = (rnd() % 100 < 75) ? 0 : (int)(rnd() % (unsigned)count);
            int pos = (qh + idx) % 400;
            Rec r = q[pos];
            CHECK(pat_ok(ring + r.off, r.n, r.tag));
            r_free(r.off);
            /* close the hole in the circular record queue */
            for (int k = idx; k > 0; k--) q[(qh + k) % 400] = q[(qh + k - 1) % 400];
            qh = (qh + 1) % 400; count--;
        }
        if (step % 100 == 0) {
            ring_check(&nlb);
            CHECK((int)nlb == count);
            for (int k = 0; k < count; k++) { Rec *r = &q[(qh + k) % 400]; CHECK(pat_ok(ring + r->off, r->n, r->tag)); }
        }
        if (step % 5000 == 0) printf("step %5d: in flight=%3d used=%4u head=%4u tail=%4u\n", step, count, used, head, tail);
    }
    ring_check(&nlb);
    printf("allocs=%lu refused=%lu wraps=%lu padding=%lu bytes\n", allocs, refused, wraps, pad_bytes);
    printf("out-of-order frees=%lu blocks reclaimed=%lu peak used=%lu of %u\n", ooo_frees, reclaimed, peak_used, CAP);
    while (count > 0) { Rec r = q[qh]; qh = (qh + 1) % 400; count--; CHECK(pat_ok(ring + r.off, r.n, r.tag)); r_free(r.off); }
    CHECK(used == 0);
    printf("all completed: used=%u\n", used);
    return 0;
}
