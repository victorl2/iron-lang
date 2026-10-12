/*
 * title: Aligned allocation carved out of an address-ordered free list
 * topic: memory
 * covers: aligned allocation, K&R style address-ordered free list, front and tail remainders returned as free blocks, alignment up to 256, coalescing on free, heap walk checker
 * deps: libc
 */
#define SEED 0xA116EDULL
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

#define NU 512u                       /* heap units */
#define UNIT 16u
#define NILU 0xFFFFFFFFu
#define M_FREE 0xF4EEu
#define M_USED 0x05EDu

static _Alignas(256) unsigned char heap[NU * UNIT];
static uint32_t free_head = 0;
static unsigned long allocs, frees, front_pieces, tail_pieces, whole_takes, merges;
static unsigned long front_units_total[6], count_by_align[6];

typedef struct { uint32_t size, next, magic, pad; } Hdr;
static Hdr get(unsigned u) { Hdr h; memcpy(&h, heap + (size_t)u * UNIT, sizeof h); return h; }
static void put(unsigned u, uint32_t size, uint32_t next, uint32_t magic) {
    Hdr h; h.size = size; h.next = next; h.magic = magic; h.pad = 0;
    memcpy(heap + (size_t)u * UNIT, &h, sizeof h);
}

static void heap_init(void) { put(0, NU, NILU, M_FREE); free_head = 0; }

static int align_index(unsigned a) { int i = 0; while ((8u << i) < a) i++; return i; }

static long aligned_alloc_h(size_t n, unsigned align) {
    if (n == 0) return -1;
    unsigned pu = (unsigned)((n + UNIT - 1) / UNIT);
    uint32_t prev = NILU;
    for (uint32_t cur = free_head; cur != NILU; prev = cur, cur = get(cur).next) {
        Hdr h = get(cur);
        size_t a = ((size_t)(cur + 1) * UNIT + align - 1) & ~((size_t)align - 1);
        unsigned au = (unsigned)(a / UNIT);
        unsigned front = au - 1 - cur;
        unsigned total = front + 1 + pu;
        if (h.size < total) continue;
        unsigned tail = h.size - total;
        unsigned blk = cur + front;
        uint32_t after = h.next;
        if (tail > 0) {
            put(blk + 1 + pu, tail, after, M_FREE);
            after = blk + 1 + pu;
            tail_pieces++;
        }
        if (front > 0) {
            put(cur, front, after, M_FREE);
            front_pieces++;
            front_units_total[align_index(align)] += front;
        } else if (prev == NILU) free_head = after;
        else { Hdr ph = get(prev); put(prev, ph.size, after, M_FREE); }
        if (front == 0 && tail == 0) whole_takes++;
        put(blk, 1 + pu, NILU, M_USED);
        allocs++; count_by_align[align_index(align)]++;
        return (long)a;
    }
    return -1;
}

static void free_h(unsigned byte_off) {
    unsigned u = byte_off / UNIT - 1;
    Hdr h = get(u);
    CHECK(h.magic == M_USED);
    frees++;
    uint32_t prev = NILU, cur = free_head;
    while (cur != NILU && cur < u) { prev = cur; cur = get(cur).next; }
    uint32_t size = h.size, next = cur;
    if (cur != NILU && u + size == cur) { size += get(cur).size; next = get(cur).next; merges++; }
    if (prev != NILU && prev + get(prev).size == u) {
        put(prev, get(prev).size + size, next, M_FREE);
        merges++;
    } else {
        put(u, size, next, M_FREE);
        if (prev == NILU) free_head = u; else put(prev, get(prev).size, u, M_FREE);
    }
}

static void heap_check(unsigned *nf, unsigned *fu, unsigned *big) {
    unsigned u = 0, cnt = 0, units = 0, mx = 0;
    int prev_free = 0;
    while (u < NU) {
        Hdr h = get(u);
        CHECK(h.size >= 1 && u + h.size <= NU && (h.magic == M_FREE || h.magic == M_USED));
        if (h.magic == M_FREE) { CHECK(!prev_free); prev_free = 1; cnt++; units += h.size; if (h.size > mx) mx = h.size; }
        else prev_free = 0;
        u += h.size;
    }
    CHECK(u == NU);
    unsigned ln = 0;
    int64_t last = -1;
    for (uint32_t c = free_head; c != NILU; c = get(c).next) {
        CHECK(get(c).magic == M_FREE && (int64_t)c > last);
        last = c; ln++;
    }
    CHECK(ln == cnt);
    *nf = cnt; *fu = units; *big = mx;
}

typedef struct { unsigned off; size_t n; unsigned tag; unsigned align; } Rec;

int main(void) {
    heap_init();
    Rec live[300];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fu, big;
    for (int step = 0; step < 15000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 300)) {
            size_t n = 1 + rnd() % 150;
            unsigned align = 8u << (rnd() % 6);
            long off = aligned_alloc_h(n, align);
            if (off < 0) { fails++; continue; }
            CHECK(((unsigned)off & (align - 1)) == 0);
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++; live[nlive].align = align;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            free_h(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 100 == 0) {
            heap_check(&nf, &fu, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fu, &big);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, fails, nlive);
    for (int a = 0; a < 6; a++)
        printf("align %3u: allocs=%5lu avg front gap=%.2f units\n", 8u << a, count_by_align[a], count_by_align[a] ? (double)front_units_total[a] / (double)count_by_align[a] : 0.0);
    printf("front pieces=%lu tail pieces=%lu whole-block takes=%lu merges=%lu\n", front_pieces, tail_pieces, whole_takes, merges);
    printf("free blocks=%u free units=%u largest=%u\n", nf, fu, big);
    for (int i = 0; i < nlive; i++) free_h(live[i].off);
    heap_check(&nf, &fu, &big);
    CHECK(nf == 1 && fu == NU && free_head == 0);
    printf("drained: one block of %u units\n", fu);
    return 0;
}
