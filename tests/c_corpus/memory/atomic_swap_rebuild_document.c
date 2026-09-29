/*
 * title: Build-aside-then-swap document rebuild with old state preserved on failure
 * topic: memory
 * covers: transactional update, partial construction rollback, failure injection, checksum of old state
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long ncalls, fail_at, live;
static void *xm(size_t n) {
    ncalls++;
    if (fail_at && ncalls == fail_at) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void xf(void *p) { if (p) { live--; free(p); } }

typedef struct { char **lines; int nlines; } Section;
typedef struct { Section *secs; int nsecs; unsigned version; } Doc;

static void doc_free(Doc *d) {
    if (!d) return;
    for (int i = 0; i < d->nsecs; i++) {
        for (int j = 0; j < d->secs[i].nlines; j++) xf(d->secs[i].lines[j]);
        xf(d->secs[i].lines);
    }
    xf(d->secs);
    xf(d);
}

/* deterministic content for (version): sections 2..4, lines 1..4 */
static Doc *doc_build(unsigned version) {
    Doc *d = xm(sizeof *d);
    if (!d) return NULL;
    d->nsecs = 0; d->secs = NULL; d->version = version;
    int nsecs = 2 + (int)(version % 3);
    d->secs = xm((size_t)nsecs * sizeof(Section));
    if (!d->secs) { xf(d); return NULL; }
    memset(d->secs, 0, (size_t)nsecs * sizeof(Section));
    d->nsecs = nsecs; /* from here doc_free cleans whatever exists */
    for (int i = 0; i < nsecs; i++) {
        int nl = 1 + (int)((version + (unsigned)i) % 4);
        d->secs[i].lines = xm((size_t)nl * sizeof(char *));
        if (!d->secs[i].lines) { d->nsecs = i; doc_free(d); return NULL; }
        d->secs[i].nlines = 0;
        for (int j = 0; j < nl; j++) {
            char *ln = xm(24);
            if (!ln) { d->nsecs = i + 1; doc_free(d); return NULL; }
            snprintf(ln, 24, "v%u s%d l%d", version, i, j);
            d->secs[i].lines[j] = ln;
            d->secs[i].nlines++;
        }
    }
    return d;
}

static uint32_t checksum(const Doc *d) {
    uint32_t h = 2166136261u;
    h = (h ^ d->version) * 16777619u;
    for (int i = 0; i < d->nsecs; i++)
        for (int j = 0; j < d->secs[i].nlines; j++)
            for (const char *c = d->secs[i].lines[j]; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    return h;
}

/* update: build the replacement first, swap only on success */
static int doc_update(Doc **slot, unsigned version) {
    Doc *nd = doc_build(version);
    if (!nd) return -1;
    Doc *old = *slot;
    *slot = nd;
    doc_free(old);
    return 0;
}

int main(void) {
    Doc *cur = doc_build(1);
    if (!cur) return 1;
    long base_live = live;
    uint32_t sum1 = checksum(cur);
    printf("v1: sections=%d checksum=%08x live=%ld\n", cur->nsecs, (unsigned)sum1, live);

    /* measure allocations needed for the v2 rebuild */
    ncalls = 0; fail_at = 0;
    if (doc_update(&cur, 2) != 0) return 1;
    long need = ncalls;
    uint32_t sum2 = checksum(cur);
    printf("v2: sections=%d checksum=%08x, rebuild took %ld allocations\n", cur->nsecs, (unsigned)sum2, need);

    /* fail each allocation of the v3 rebuild: v2 must survive bit-for-bit */
    ncalls = 0; fail_at = 0;
    Doc *probe = doc_build(3);
    long need3 = ncalls;
    doc_free(probe);
    long live_v2 = live;
    int survived = 0;
    for (long n = 1; n <= need3; n++) {
        fail_at = n; ncalls = 0;
        int rc = doc_update(&cur, 3);
        if (rc == 0 || checksum(cur) != sum2 || cur->version != 2 || live != live_v2) {
            fprintf(stderr, "old doc damaged at %ld\n", n);
            return 1;
        }
        survived++;
    }
    fail_at = 0;
    printf("v3 rebuild needs %ld allocations; %d injected failures left v2 intact\n", need3, survived);
    if (doc_update(&cur, 3) != 0) return 1;
    printf("v3: sections=%d checksum=%08x live=%ld\n", cur->nsecs, (unsigned)checksum(cur), live);
    doc_free(cur);
    printf("final live=%ld (base was %ld)\n", live, base_live);
    return live == 0 ? 0 : 1;
}
