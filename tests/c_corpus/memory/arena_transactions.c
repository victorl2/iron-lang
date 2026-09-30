/*
 * title: Arena-backed append-only log with nested transactions and checkpoint compaction
 * topic: memory
 * covers: transactions as arena marks, rollback by truncation, commit merging into parent, newest-first lookup, log compaction into a scratch buffer, snapshot model cross-check
 * deps: libc
 */
#define SEED 0x7A25AC7ULL
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

#define CAP 6144u
#define NKEYS 48
#define MAXENT 1024
#define MAXDEPTH 6
#define HDRB 16u
enum { K_SET = 1, K_DEL = 2 };

static _Alignas(16) unsigned char arena[CAP];
static unsigned used, peak, nent;
static unsigned ent_off[MAXENT];

typedef struct { unsigned used, nent; } Mark;
static Mark marks[MAXDEPTH];
static int depth;

typedef struct { int present; unsigned vlen, tag; } Cell;
static Cell model[NKEYS];
static Cell snap[MAXDEPTH][NKEYS];

static unsigned long n_set, n_del, n_commit, n_rollback, n_checkpoint, scanned, refused, deepest;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, arena + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(arena + o, &v, 4); }

static int append(unsigned kind, unsigned key, unsigned vlen, unsigned tag) {
    unsigned need = HDRB + ((vlen + 3u) & ~3u);
    if (used + need > CAP || nent == MAXENT) return 0;
    unsigned o = used;
    wr(o, kind); wr(o + 4, key); wr(o + 8, vlen); wr(o + 12, tag);
    if (vlen) pat_fill(arena + o + HDRB, vlen, tag);
    ent_off[nent++] = o;
    used += need;
    if (used > peak) peak = used;
    return 1;
}

/* newest entry for a key wins; a DEL entry means absent */
static int lookup(unsigned key, unsigned *vlen, unsigned *tag) {
    for (unsigned i = nent; i-- > 0; ) {
        scanned++;
        unsigned o = ent_off[i];
        if (rd(o + 4) != key) continue;
        if (rd(o) == K_DEL) return 0;
        *vlen = rd(o + 8); *tag = rd(o + 12);
        CHECK(pat_ok(arena + o + HDRB, *vlen, *tag));
        return 1;
    }
    return 0;
}

static void checkpoint(void) {
    CHECK(depth == 0);
    static unsigned char scratch[CAP];
    unsigned su = 0, sn = 0;
    static unsigned new_off[MAXENT];
    for (unsigned key = 0; key < NKEYS; key++) {
        unsigned vl, tg;
        if (!lookup(key, &vl, &tg)) continue;
        unsigned need = HDRB + ((vl + 3u) & ~3u);
        uint32_t hdr[4] = { K_SET, key, vl, tg };
        memcpy(scratch + su, hdr, sizeof hdr);
        pat_fill(scratch + su + HDRB, vl, tg);
        new_off[sn++] = su;
        su += need;
    }
    memset(arena, 0, CAP);
    memcpy(arena, scratch, su);
    memcpy(ent_off, new_off, sn * sizeof(unsigned));
    used = su; nent = sn;
    n_checkpoint++;
}

static void begin(void) {
    CHECK(depth < MAXDEPTH);
    marks[depth].used = used; marks[depth].nent = nent;
    memcpy(snap[depth], model, sizeof model);
    depth++;
    if ((unsigned long)depth > deepest) deepest = (unsigned long)depth;
}
static void commit(void) { CHECK(depth > 0); depth--; n_commit++; }
static void rollback(void) {
    CHECK(depth > 0);
    depth--;
    memset(arena + marks[depth].used, 0xDD, used - marks[depth].used);
    used = marks[depth].used; nent = marks[depth].nent;
    memcpy(model, snap[depth], sizeof model);
    n_rollback++;
}

static unsigned next_tag = 1;

static void do_set(unsigned key) {
    unsigned vl = 1 + rnd() % 60;
    unsigned tg = next_tag++;
    if (!append(K_SET, key, vl, tg)) {
        if (depth == 0) { checkpoint(); if (!append(K_SET, key, vl, tg)) { refused++; return; } }
        else { refused++; return; }
    }
    model[key].present = 1; model[key].vlen = vl; model[key].tag = tg;
    n_set++;
}
static void do_del(unsigned key) {
    if (!append(K_DEL, key, 0, 0)) { refused++; return; }
    model[key].present = 0;
    n_del++;
}

static void verify_all(void) {
    for (unsigned k = 0; k < NKEYS; k++) {
        unsigned vl = 0, tg = 0;
        int p = lookup(k, &vl, &tg);
        CHECK(p == model[k].present);
        if (p) CHECK(vl == model[k].vlen && tg == model[k].tag);
    }
}

int main(void) {
    for (int step = 0; step < 30000; step++) {
        unsigned op = rnd() % 100;
        unsigned key = rnd() % NKEYS;
        if (op < 50) do_set(key);
        else if (op < 62) do_del(key);
        else if (op < 74) { if (depth < MAXDEPTH) begin(); }
        else if (op < 84) { if (depth > 0) commit(); }
        else if (op < 95) { if (depth > 0) rollback(); }
        else if (depth == 0) checkpoint();
        if (step % 50 == 0) verify_all();
    }
    while (depth > 0) commit();
    verify_all();
    unsigned live = 0, live_bytes = 0;
    for (unsigned k = 0; k < NKEYS; k++) if (model[k].present) { live++; live_bytes += model[k].vlen; }
    printf("sets=%lu dels=%lu refused=%lu\n", n_set, n_del, refused);
    printf("commits=%lu rollbacks=%lu deepest nesting=%lu checkpoints=%lu\n", n_commit, n_rollback, deepest, n_checkpoint);
    printf("log entries scanned by lookups=%lu, arena peak=%u of %u\n", scanned, peak, CAP);
    printf("live keys=%u (%u value bytes), log now %u entries in %u bytes\n", live, live_bytes, nent, used);
    checkpoint();
    verify_all();
    CHECK(nent == live);
    printf("after final checkpoint: %u entries, %u bytes\n", nent, used);
    return 0;
}
