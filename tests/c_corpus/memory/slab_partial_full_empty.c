/*
 * title: Slab allocator with partial, full and empty lists
 * topic: memory
 * covers: slab allocator, per-slab freelist, partial/full/empty list transitions, slab reaping, slab lookup from pointer
 * deps: libc
 */
#define SEED 0x51ABA110C8ULL
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

#define SLAB_SIZE 512
#define NSLABS 16
#define OBJ 48
#define HDR 32
#define PER_SLAB ((SLAB_SIZE - HDR) / OBJ)

static _Alignas(16) unsigned char mem[SLAB_SIZE * NSLABS];

enum { L_NONE, L_PARTIAL, L_FULL, L_EMPTY };
typedef struct {
    int list, prev, next;
    int inuse, free_head; /* free_head is an object index within the slab */
    int created;
} Slab;

static Slab slab[NSLABS];
static int head[4];
static int slabs_created, slabs_reaped, empty_count, page_free[NSLABS], pages_free;
static int allocs, frees;
static const char *list_name[] = { "none", "partial", "full", "empty" };

static void list_remove(int s) {
    Slab *x = &slab[s];
    if (x->prev >= 0) slab[x->prev].next = x->next; else head[x->list] = x->next;
    if (x->next >= 0) slab[x->next].prev = x->prev;
    if (x->list == L_EMPTY) empty_count--;
    x->list = L_NONE; x->prev = x->next = -1;
}
static void list_push(int s, int l) {
    Slab *x = &slab[s];
    x->list = l; x->prev = -1; x->next = head[l];
    if (head[l] >= 0) slab[head[l]].prev = s;
    head[l] = s;
    if (l == L_EMPTY) empty_count++;
}

static unsigned char *obj_at(int s, int i) { return mem + (size_t)s * SLAB_SIZE + HDR + (size_t)i * OBJ; }

static int slab_create(void) {
    if (pages_free == 0) return -1;
    int s = page_free[--pages_free];
    Slab *x = &slab[s];
    x->inuse = 0; x->free_head = 0; x->created++;
    for (int i = 0; i < PER_SLAB; i++) {
        int nx = i + 1 < PER_SLAB ? i + 1 : -1;
        memcpy(obj_at(s, i), &nx, sizeof nx);
    }
    x->list = L_NONE;
    list_push(s, L_EMPTY);
    slabs_created++;
    return s;
}

static void *cache_alloc(void) {
    int s = head[L_PARTIAL];
    if (s < 0) s = head[L_EMPTY];
    if (s < 0) { s = slab_create(); if (s < 0) return NULL; }
    Slab *x = &slab[s];
    int i = x->free_head, nx;
    unsigned char *p = obj_at(s, i);
    memcpy(&nx, p, sizeof nx);
    x->free_head = nx;
    x->inuse++;
    list_remove(s);
    list_push(s, x->inuse == PER_SLAB ? L_FULL : L_PARTIAL);
    allocs++;
    return p;
}

static void reap_extra_empties(void) {
    while (empty_count > 1) {
        int s = head[L_EMPTY];
        list_remove(s);
        page_free[pages_free++] = s;
        slabs_reaped++;
    }
}

static void cache_free(void *p) {
    size_t off = (size_t)((unsigned char *)p - mem);
    int s = (int)(off / SLAB_SIZE);
    size_t rel = off % SLAB_SIZE;
    CHECK(s < NSLABS && rel >= HDR && (rel - HDR) % OBJ == 0);
    int i = (int)((rel - HDR) / OBJ);
    Slab *x = &slab[s];
    CHECK(x->list != L_NONE && x->inuse > 0);
    memcpy(p, &x->free_head, sizeof(int));
    x->free_head = i;
    x->inuse--;
    list_remove(s);
    list_push(s, x->inuse == 0 ? L_EMPTY : L_PARTIAL);
    frees++;
    reap_extra_empties();
}

static void slab_check(void) {
    int in_lists = 0, objs = 0;
    for (int l = 1; l <= 3; l++) {
        int prev = -1;
        for (int s = head[l]; s >= 0; s = slab[s].next) {
            CHECK(slab[s].list == l && slab[s].prev == prev);
            prev = s; in_lists++;
            int fc = 0;
            for (int i = slab[s].free_head; i >= 0; ) {
                fc++; CHECK(fc <= PER_SLAB);
                memcpy(&i, obj_at(s, i), sizeof i);
            }
            CHECK(fc + slab[s].inuse == PER_SLAB);
            if (l == L_FULL) CHECK(slab[s].inuse == PER_SLAB);
            if (l == L_EMPTY) CHECK(slab[s].inuse == 0);
            if (l == L_PARTIAL) CHECK(slab[s].inuse > 0 && slab[s].inuse < PER_SLAB);
            objs += slab[s].inuse;
        }
    }
    CHECK(in_lists + pages_free == NSLABS);
    CHECK(objs == allocs - frees);
}

typedef struct { unsigned char *p; unsigned tag; } Rec;

int main(void) {
    for (int i = 0; i < NSLABS; i++) { slab[i].list = L_NONE; slab[i].prev = slab[i].next = -1; page_free[i] = NSLABS - 1 - i; }
    pages_free = NSLABS;
    head[1] = head[2] = head[3] = -1;
    static Rec live[NSLABS * PER_SLAB];
    int nlive = 0, refused = 0, max_full = 0, phase_peak[4] = {0};
    unsigned tag = 1;
    /* four phases with changing alloc/free bias to push slabs through all states */
    static const int bias[4] = { 75, 30, 60, 10 };
    for (int phase = 0; phase < 4; phase++) {
        for (int step = 0; step < 1500; step++) {
            if (nlive == 0 || (int)(rnd() % 100) < bias[phase]) {
                unsigned char *p = cache_alloc();
                if (!p) { refused++; continue; }
                pat_fill(p + 4, OBJ - 4, tag);
                live[nlive].p = p; live[nlive].tag = tag++;
                nlive++;
            } else {
                int i = (int)(rnd() % (unsigned)nlive);
                CHECK(pat_ok(live[i].p + 4, OBJ - 4, live[i].tag));
                cache_free(live[i].p);
                live[i] = live[--nlive];
            }
            if (nlive > phase_peak[phase]) phase_peak[phase] = nlive;
            int nf = 0;
            for (int s = head[L_FULL]; s >= 0; s = slab[s].next) nf++;
            if (nf > max_full) max_full = nf;
            if (step % 50 == 0) slab_check();
        }
        slab_check();
        int np = 0, nf = 0, ne = 0;
        for (int s = head[L_PARTIAL]; s >= 0; s = slab[s].next) np++;
        for (int s = head[L_FULL]; s >= 0; s = slab[s].next) nf++;
        for (int s = head[L_EMPTY]; s >= 0; s = slab[s].next) ne++;
        printf("phase %d: live=%3d peak=%3d slabs partial=%d full=%d empty=%d idle=%d\n", phase, nlive, phase_peak[phase], np, nf, ne, pages_free);
    }
    printf("objects per slab=%d created=%d reaped=%d refused=%d max full=%d\n", PER_SLAB, slabs_created, slabs_reaped, refused, max_full);
    for (int i = 0; i < nlive; i++) cache_free(live[i].p);
    slab_check();
    CHECK(head[L_FULL] < 0 && head[L_PARTIAL] < 0);
    printf("after drain: empty=%d idle=%d (%s list only)\n", empty_count, pages_free, list_name[L_EMPTY]);
    return 0;
}
