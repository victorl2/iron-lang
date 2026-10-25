/*
 * title: Extent allocator for files with growth, truncation and defragmentation
 * topic: memory
 * covers: extent lists, sorted free extent table with merging, best-fit single extent then multi-extent fallback, logical to physical mapping, defragmentation by copy, ownership map cross-check
 * deps: libc
 */
#define SEED 0xE47E27ULL
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

#define NB 1024u              /* blocks on the "disk" */
#define BS 16u
#define MAXFILES 24
#define MAXEXT 8
#define MAXFREE 512

typedef struct { unsigned start, len; } Ext;
typedef struct { int exists; int next; Ext ext[MAXEXT]; unsigned nblocks; unsigned gen; } File;

static _Alignas(16) unsigned char disk[NB * BS];
static Ext freex[MAXFREE];
static int nfree;
static File files[MAXFILES];
static int owner[NB];                        /* file index + 1, 0 = free */
static unsigned long grows_single, grows_multi, grow_fail, defrags, defrag_skipped, truncs, deletes, blocks_copied;

static void free_insert(unsigned start, unsigned len) {
    int i = 0;
    while (i < nfree && freex[i].start < start) i++;
    CHECK(nfree < MAXFREE);
    memmove(&freex[i + 1], &freex[i], (size_t)(nfree - i) * sizeof(Ext));
    freex[i].start = start; freex[i].len = len; nfree++;
    if (i + 1 < nfree && freex[i].start + freex[i].len == freex[i + 1].start) {
        freex[i].len += freex[i + 1].len;
        memmove(&freex[i + 1], &freex[i + 2], (size_t)(nfree - i - 2) * sizeof(Ext));
        nfree--;
    }
    if (i > 0 && freex[i - 1].start + freex[i - 1].len == freex[i].start) {
        freex[i - 1].len += freex[i].len;
        memmove(&freex[i], &freex[i + 1], (size_t)(nfree - i - 1) * sizeof(Ext));
        nfree--;
    }
}

/* take `len` blocks from the front of free extent i */
static Ext take_from(int i, unsigned len) {
    Ext e; e.start = freex[i].start; e.len = len;
    if (freex[i].len == len) { memmove(&freex[i], &freex[i + 1], (size_t)(nfree - i - 1) * sizeof(Ext)); nfree--; }
    else { freex[i].start += len; freex[i].len -= len; }
    return e;
}

static int best_fit_index(unsigned len) {
    int best = -1;
    for (int i = 0; i < nfree; i++) if (freex[i].len >= len && (best < 0 || freex[i].len < freex[best].len)) best = i;
    return best;
}
static int largest_index(void) {
    int b = -1;
    for (int i = 0; i < nfree; i++) if (b < 0 || freex[i].len > freex[b].len) b = i;
    return b;
}
static unsigned free_total(void) { unsigned t = 0; for (int i = 0; i < nfree; i++) t += freex[i].len; return t; }

static unsigned phys_block(const File *f, unsigned logical) {
    for (int i = 0; i < f->next; i++) {
        if (logical < f->ext[i].len) return f->ext[i].start + logical;
        logical -= f->ext[i].len;
    }
    CHECK(0);
    return 0;
}

static void stamp_block(int fi, unsigned logical) {
    unsigned pb = phys_block(&files[fi], logical);
    pat_fill(disk + (size_t)pb * BS, BS, (unsigned)fi * 4096u + logical * 8u + files[fi].gen);
}
static int check_block(int fi, unsigned logical) {
    unsigned pb = phys_block(&files[fi], logical);
    return pat_ok(disk + (size_t)pb * BS, BS, (unsigned)fi * 4096u + logical * 8u + files[fi].gen);
}

static void claim(int fi, Ext e) {
    File *f = &files[fi];
    for (unsigned b = e.start; b < e.start + e.len; b++) { CHECK(owner[b] == 0); owner[b] = fi + 1; }
    if (f->next > 0 && f->ext[f->next - 1].start + f->ext[f->next - 1].len == e.start) f->ext[f->next - 1].len += e.len;
    else { CHECK(f->next < MAXEXT); f->ext[f->next++] = e; }
}

/* append n blocks to file fi; all-or-nothing */
static int grow(int fi, unsigned n) {
    File *f = &files[fi];
    if (free_total() < n) { grow_fail++; return 0; }
    int i = best_fit_index(n);
    unsigned old_blocks = f->nblocks;
    if (i >= 0) {
        int room = f->next < MAXEXT || (f->next > 0 && f->ext[f->next - 1].start + f->ext[f->next - 1].len == freex[i].start);
        if (room) { claim(fi, take_from(i, n)); f->nblocks += n; grows_single++; goto stamp; }
    }
    /* fall back: largest free extents first; refuse if the file would run out of extent slots */
    {
        Ext got[MAXEXT];
        int ng = 0;
        unsigned need = n;
        while (need > 0 && ng < MAXEXT && nfree > 0) {
            int j = largest_index();
            unsigned take = freex[j].len < need ? freex[j].len : need;
            got[ng++] = take_from(j, take);
            need -= take;
        }
        if (need > 0 || f->next + ng > MAXEXT) {
            for (int k = 0; k < ng; k++) free_insert(got[k].start, got[k].len);
            grow_fail++;
            return 0;
        }
        for (int k = 0; k < ng; k++) claim(fi, got[k]);
        f->nblocks += n;
        grows_multi++;
    }
stamp:
    for (unsigned l = old_blocks; l < f->nblocks; l++) stamp_block(fi, l);
    return 1;
}

static void release_range(int fi, unsigned start, unsigned len) {
    for (unsigned b = start; b < start + len; b++) { CHECK(owner[b] == fi + 1); owner[b] = 0; }
    memset(disk + (size_t)start * BS, 0xDD, (size_t)len * BS);
    free_insert(start, len);
}

static void truncate_to(int fi, unsigned keep) {
    File *f = &files[fi];
    while (f->nblocks > keep) {
        Ext *e = &f->ext[f->next - 1];
        unsigned drop = f->nblocks - keep;
        if (drop >= e->len) { release_range(fi, e->start, e->len); f->nblocks -= e->len; f->next--; }
        else { release_range(fi, e->start + e->len - drop, drop); e->len -= drop; f->nblocks -= drop; }
    }
    truncs++;
}

static void delete_file(int fi) { truncate_to(fi, 0); files[fi].exists = 0; deletes++; }

static int defrag(int fi) {
    File *f = &files[fi];
    if (f->next <= 1) { defrag_skipped++; return 0; }
    int i = best_fit_index(f->nblocks);
    if (i < 0) { defrag_skipped++; return 0; }
    Ext dst = take_from(i, f->nblocks);
    unsigned char *tmp = malloc((size_t)f->nblocks * BS);
    CHECK(tmp);
    for (unsigned l = 0; l < f->nblocks; l++) memcpy(tmp + (size_t)l * BS, disk + (size_t)phys_block(f, l) * BS, BS);
    for (int k = 0; k < f->next; k++) release_range(fi, f->ext[k].start, f->ext[k].len);
    for (unsigned b = dst.start; b < dst.start + dst.len; b++) { CHECK(owner[b] == 0); owner[b] = fi + 1; }
    memcpy(disk + (size_t)dst.start * BS, tmp, (size_t)f->nblocks * BS);
    free(tmp);
    f->ext[0] = dst; f->next = 1;
    blocks_copied += f->nblocks;
    defrags++;
    return 1;
}

static void audit(void) {
    unsigned owned = 0;
    for (unsigned b = 0; b < NB; b++) owned += owner[b] != 0;
    unsigned blocks = 0;
    for (int fi = 0; fi < MAXFILES; fi++) {
        File *f = &files[fi];
        if (!f->exists) { CHECK(f->nblocks == 0 && f->next == 0); continue; }
        unsigned sum = 0;
        for (int k = 0; k < f->next; k++) {
            CHECK(f->ext[k].len > 0);
            for (unsigned b = f->ext[k].start; b < f->ext[k].start + f->ext[k].len; b++) CHECK(owner[b] == fi + 1);
            sum += f->ext[k].len;
        }
        CHECK(sum == f->nblocks);
        for (unsigned l = 0; l < f->nblocks; l++) CHECK(check_block(fi, l));
        blocks += sum;
    }
    CHECK(blocks == owned);
    CHECK(owned + free_total() == NB);
    for (int i = 0; i < nfree; i++) {
        CHECK(freex[i].len > 0 && (i == 0 || freex[i - 1].start + freex[i - 1].len < freex[i].start));
        for (unsigned b = freex[i].start; b < freex[i].start + freex[i].len; b++) CHECK(owner[b] == 0);
    }
}

int main(void) {
    free_insert(0, NB);
    int hist_max = 0;
    for (int step = 0; step < 6000; step++) {
        int fi = (int)(rnd() % MAXFILES);
        unsigned op = rnd() % 100;
        File *f = &files[fi];
        if (!f->exists) {
            if (op < 40) { f->exists = 1; f->gen = rnd() % 8; if (!grow(fi, 1 + rnd() % 60)) f->exists = 0; }
        } else if (op < 45) {
            grow(fi, 1 + rnd() % 40);
        } else if (op < 58) {
            if (f->nblocks > 1) truncate_to(fi, 1 + rnd() % (f->nblocks - 1));
        } else if (op < 66) {
            delete_file(fi);
        } else {
            defrag(fi);
        }
        if (f->exists && f->next > hist_max) hist_max = f->next;
        if (step % 100 == 0) audit();
    }
    audit();
    int hist[MAXEXT + 1] = {0}, nf_files = 0;
    unsigned used = 0;
    for (int fi = 0; fi < MAXFILES; fi++) if (files[fi].exists) { hist[files[fi].next]++; nf_files++; used += files[fi].nblocks; }
    printf("grows: single extent=%lu multi extent=%lu failed=%lu\n", grows_single, grows_multi, grow_fail);
    printf("truncates=%lu deletes=%lu defrags=%lu skipped=%lu blocks copied=%lu\n", truncs, deletes, defrags, defrag_skipped, blocks_copied);
    printf("files=%d blocks used=%u free=%u in %d extents, largest free=%u\n", nf_files, used, free_total(), nfree, nfree ? freex[largest_index()].len : 0);
    printf("extents per file (1..%d):", MAXEXT);
    for (int k = 1; k <= MAXEXT; k++) printf(" %d", hist[k]);
    printf("\nmax extents seen on one file: %d\n", hist_max);
    for (int fi = 0; fi < MAXFILES; fi++) if (files[fi].exists) delete_file(fi);
    audit();
    CHECK(nfree == 1 && freex[0].len == NB);
    printf("all files deleted: one free extent of %u blocks\n", freex[0].len);
    return 0;
}
