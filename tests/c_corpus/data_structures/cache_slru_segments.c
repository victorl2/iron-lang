/*
 * title: Segmented LRU with probationary and protected segments
 * topic: data_structures
 * covers: SLRU, two segments, promotion on second hit, demotion on overflow, segment ratio sweep, value store, explicit invalidate, array oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C 30
#define KEYS 300

static unsigned long long rs = 0x5142ULL * 0x9E3779B97ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

enum { NONE, PROB, PROT };
typedef struct {
    int prot_cap;
    int seg[KEYS], pv[KEYS], nx[KEYS], val[KEYS];
    int hd[3], tl[3], ln[3];
    long hits, misses;
} Slru;

static void push(Slru *s, int l, int k) { s->pv[k] = -1; s->nx[k] = s->hd[l]; if (s->hd[l] >= 0) s->pv[s->hd[l]] = k; else s->tl[l] = k; s->hd[l] = k; s->seg[k] = l; s->ln[l]++; }
static void rem(Slru *s, int k) {
    int l = s->seg[k];
    if (s->pv[k] >= 0) s->nx[s->pv[k]] = s->nx[k]; else s->hd[l] = s->nx[k];
    if (s->nx[k] >= 0) s->pv[s->nx[k]] = s->pv[k]; else s->tl[l] = s->pv[k];
    s->ln[l]--; s->seg[k] = NONE;
}
static void slru_init(Slru *s, int prot_cap) {
    memset(s, 0, sizeof *s); s->prot_cap = prot_cap;
    for (int l = 0; l < 3; l++) s->hd[l] = s->tl[l] = -1;
}
static int slru_get(Slru *s, int k, int *v) {
    if (s->seg[k] == NONE) { s->misses++; return 0; }
    s->hits++;
    if (s->seg[k] == PROT) rem(s, k), push(s, PROT, k);
    else {
        rem(s, k); push(s, PROT, k);
        if (s->ln[PROT] > s->prot_cap) { int d = s->tl[PROT]; rem(s, d); push(s, PROB, d); }   /* demote, keeps size */
    }
    *v = s->val[k];
    return 1;
}
static void slru_put(Slru *s, int k, int v) {
    if (s->seg[k] != NONE) { int dummy; s->val[k] = v; slru_get(s, k, &dummy); s->hits--; return; }
    if (s->ln[PROB] + s->ln[PROT] == C) {
        int victim = s->ln[PROB] ? s->tl[PROB] : s->tl[PROT];
        rem(s, victim);
    }
    s->val[k] = v; push(s, PROB, k);
}
static void slru_del(Slru *s, int k) { if (s->seg[k] != NONE) rem(s, k); }

/* oracle with arrays, front = MRU */
typedef struct { int a[3][C + 2], n[3], val[KEYS], prot_cap; } Ref;
static int rfind(Ref *r, int l, int k) { for (int i = 0; i < r->n[l]; i++) if (r->a[l][i] == k) return i; return -1; }
static void rdel(Ref *r, int l, int i) { memmove(&r->a[l][i], &r->a[l][i + 1], (size_t)(r->n[l] - i - 1) * sizeof(int)); r->n[l]--; }
static void radd(Ref *r, int l, int k) { memmove(&r->a[l][1], &r->a[l][0], (size_t)r->n[l] * sizeof(int)); r->a[l][0] = k; r->n[l]++; }
static int rtouch(Ref *r, int k) {
    int i = rfind(r, PROT, k);
    if (i >= 0) { rdel(r, PROT, i); radd(r, PROT, k); return 1; }
    i = rfind(r, PROB, k);
    if (i < 0) return 0;
    rdel(r, PROB, i); radd(r, PROT, k);
    if (r->n[PROT] > r->prot_cap) { int d = r->a[PROT][r->n[PROT] - 1]; rdel(r, PROT, r->n[PROT] - 1); radd(r, PROB, d); }
    return 1;
}
static void rput(Ref *r, int k, int v) {
    if (rtouch(r, k)) { r->val[k] = v; return; }
    if (r->n[PROB] + r->n[PROT] == C) { if (r->n[PROB]) rdel(r, PROB, r->n[PROB] - 1); else rdel(r, PROT, r->n[PROT] - 1); }
    r->val[k] = v; radd(r, PROB, k);
}

int main(void) {
    int caps[4] = {0, 10, 24, 30};
    printf("%-8s %8s %8s\n", "protcap", "hits", "hit%");
    for (int ci = 0; ci < 4; ci++) {
        Slru s; slru_init(&s, caps[ci]);
        Ref *r = calloc(1, sizeof *r); r->prot_cap = caps[ci];
        rs = 0x5142ULL * 77;    /* same workload for every configuration */
        int scan = 0;
        for (int step = 0; step < 30000; step++) {
            int k; unsigned x = rnd() % 100, op = rnd() % 25;
            if (x < 55) k = (int)(rnd() % 14); else if (x < 75) k = 14 + (int)(rnd() % 46); else k = 60 + (scan++ % 240);
            if (op == 0) { slru_del(&s, k); int i;
                if ((i = rfind(r, PROT, k)) >= 0) rdel(r, PROT, i); else if ((i = rfind(r, PROB, k)) >= 0) rdel(r, PROB, i); }
            else {
                int v = 0, got = slru_get(&s, k, &v), rg = rtouch(r, k);
                check(got == rg, "hit agrees");
                if (got) check(v == r->val[k], "value agrees");
                else { slru_put(&s, k, step); rput(r, k, step); }
            }
            check(s.ln[PROB] == r->n[PROB] && s.ln[PROT] == r->n[PROT], "segment sizes");
            check(s.ln[PROT] <= caps[ci] && s.ln[PROB] + s.ln[PROT] <= C, "capacity");
        }
        for (int l = PROB; l <= PROT; l++) { int i = 0; for (int k = s.hd[l]; k >= 0; k = s.nx[k], i++) check(r->a[l][i] == k, "segment order"); }
        printf("%-8d %8ld %7ld%%\n", caps[ci], s.hits, s.hits * 100 / (s.hits + s.misses));
        free(r);
    }
    return 0;
}
