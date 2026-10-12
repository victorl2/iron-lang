/*
 * title: LIFO stack allocator with headers, markers and top resize
 * topic: memory
 * covers: stack allocator, per-block back-offset header, alignment padding, LIFO enforcement, markers with bulk release, in-place resize of top block
 * deps: libc
 */
#define SEED 0x57AC4A110CULL
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

#define CAP 4096u

static _Alignas(16) unsigned char buf[CAP];
static unsigned top;                     /* offset of first unused byte */
static unsigned peak, pushes, pops, rejected_pops, resizes, marker_releases, refused;

/* each block: [ ... padding ... ][ u32 delta ] payload
 * delta = payload_offset - top_before, so popping restores the previous top exactly */
static long st_push(size_t n, unsigned align) {
    CHECK(align && (align & (align - 1)) == 0);
    unsigned raw = top + 4;
    unsigned p = (raw + align - 1) & ~(align - 1);
    if ((size_t)p + n > CAP) { refused++; return -1; }
    uint32_t delta = p - top;
    memcpy(buf + p - 4, &delta, 4);
    top = p + (unsigned)n;
    if (top > peak) peak = top;
    pushes++;
    return (long)p;
}

/* pop requires the payload to be the top block: its end must equal top */
static int st_pop(unsigned payload, size_t n) {
    if (payload + n != top) { rejected_pops++; return -1; }
    uint32_t delta;
    memcpy(&delta, buf + payload - 4, 4);
    top = payload - delta;
    pops++;
    return 0;
}

static int st_resize_top(unsigned payload, size_t old_n, size_t new_n) {
    if (payload + old_n != top || (size_t)payload + new_n > CAP) return -1;
    top = payload + (unsigned)new_n;
    if (top > peak) peak = top;
    resizes++;
    return 0;
}

static unsigned st_marker(void) { return top; }
static void st_release_to(unsigned m) {
    CHECK(m <= top);
    memset(buf + m, 0xDD, top - m);
    top = m;
    marker_releases++;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    Rec live[400];
    int nlive = 0;
    unsigned markers[8], marker_live[8];
    int nmark = 0;
    unsigned tag = 1;
    for (int step = 0; step < 15000; step++) {
        unsigned op = rnd() % 100;
        if (op < 50) {
            size_t n = 1 + rnd() % 90;
            unsigned align = 1u << (rnd() % 5);
            long off = st_push(n, align);
            if (off >= 0) {
                CHECK(((unsigned)off & (align - 1)) == 0);
                pat_fill(buf + off, n, tag);
                live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
                nlive++;
                CHECK(nlive < 400);
            }
        } else if (op < 78) {
            if (nlive) {
                Rec *r = &live[nlive - 1];
                CHECK(pat_ok(buf + r->off, r->n, r->tag));
                CHECK(st_pop(r->off, r->n) == 0);
                nlive--;
                while (nmark > 0 && marker_live[nmark - 1] > (unsigned)nlive) nmark--;
            }
        } else if (op < 84) {
            /* popping a non-top block must be rejected without side effects */
            if (nlive >= 2) {
                unsigned t = top;
                Rec *r = &live[rnd() % (unsigned)(nlive - 1)];
                CHECK(st_pop(r->off, r->n) == -1);
                CHECK(top == t);
            }
        } else if (op < 90) {
            if (nlive && (nmark == 0 || marker_live[nmark - 1] < (unsigned)nlive)) {
                Rec *r = &live[nlive - 1];
                size_t nn = 1 + rnd() % 120;
                if (st_resize_top(r->off, r->n, nn) == 0) {
                    r->n = nn;
                    pat_fill(buf + r->off, nn, r->tag);
                }
            }
        } else if (op < 95) {
            if (nmark < 8) { markers[nmark] = st_marker(); marker_live[nmark] = (unsigned)nlive; nmark++; }
        } else if (nmark > 0) {
            nmark--;
            st_release_to(markers[nmark]);
            nlive = (int)marker_live[nmark];
            while (nmark > 0 && marker_live[nmark - 1] > (unsigned)nlive) nmark--;
        }
        if (step % 100 == 0) {
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(buf + live[i].off, live[i].n, live[i].tag));
            for (int i = 1; i < nlive; i++) CHECK(live[i - 1].off + live[i - 1].n + 4 <= live[i].off);
            if (nlive) CHECK(live[nlive - 1].off + live[nlive - 1].n == top);
            else CHECK(top == 0 || nmark > 0 || top == 0);
        }
    }
    printf("pushes=%u pops=%u refused (full)=%u\n", pushes, pops, refused);
    printf("non-top pops rejected=%u resizes=%u marker releases=%u\n", rejected_pops, resizes, marker_releases);
    printf("peak=%u of %u, top now=%u, live=%d\n", peak, CAP, top, nlive);
    while (nlive > 0) { Rec *r = &live[--nlive]; CHECK(pat_ok(buf + r->off, r->n, r->tag)); CHECK(st_pop(r->off, r->n) == 0); }
    CHECK(top == 0);
    printf("unwound to empty stack\n");
    return 0;
}
