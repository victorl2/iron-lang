/*
 * title: Linked lists in an index-based node pool
 * topic: data_structures
 * covers: index links, node pool, freelist by index, multiple lists in one arena, compaction, pointer-free lists
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x59F111F1B605D019ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 22); }

#define NIL ((int16_t)-1)
#define POOL 256
#define LISTS 4

typedef struct { int32_t val; int16_t next, prev; } Cell;
typedef struct {
    Cell cell[POOL];
    int16_t free_head;
    int16_t head[LISTS], tail[LISTS];
    int used, peak;
} Pool;

static void pool_init(Pool *p) {
    for (int i = 0; i < POOL; i++) { p->cell[i].next = (int16_t)(i + 1 < POOL ? i + 1 : -1); p->cell[i].prev = NIL; p->cell[i].val = 0; }
    p->free_head = 0; p->used = p->peak = 0;
    for (int l = 0; l < LISTS; l++) p->head[l] = p->tail[l] = NIL;
}
static int16_t alloc_cell(Pool *p) {
    if (p->free_head == NIL) return NIL;
    int16_t c = p->free_head; p->free_head = p->cell[c].next;
    p->used++; if (p->used > p->peak) p->peak = p->used;
    return c;
}
static void free_cell(Pool *p, int16_t c) { p->cell[c].next = p->free_head; p->cell[c].prev = NIL; p->free_head = c; p->used--; }
static int push_back(Pool *p, int l, int v) {
    int16_t c = alloc_cell(p); if (c == NIL) return 0;
    p->cell[c].val = v; p->cell[c].next = NIL; p->cell[c].prev = p->tail[l];
    if (p->tail[l] != NIL) p->cell[p->tail[l]].next = c; else p->head[l] = c;
    p->tail[l] = c; return 1;
}
static int push_front(Pool *p, int l, int v) {
    int16_t c = alloc_cell(p); if (c == NIL) return 0;
    p->cell[c].val = v; p->cell[c].prev = NIL; p->cell[c].next = p->head[l];
    if (p->head[l] != NIL) p->cell[p->head[l]].prev = c; else p->tail[l] = c;
    p->head[l] = c; return 1;
}
static void unlink_cell(Pool *p, int l, int16_t c) {
    int16_t pv = p->cell[c].prev, nx = p->cell[c].next;
    if (pv != NIL) p->cell[pv].next = nx; else p->head[l] = nx;
    if (nx != NIL) p->cell[nx].prev = pv; else p->tail[l] = pv;
    free_cell(p, c);
}
static int pop_front(Pool *p, int l, int *v) { if (p->head[l] == NIL) return 0; int16_t c = p->head[l]; *v = p->cell[c].val; unlink_cell(p, l, c); return 1; }
static int pop_back(Pool *p, int l, int *v) { if (p->tail[l] == NIL) return 0; int16_t c = p->tail[l]; *v = p->cell[c].val; unlink_cell(p, l, c); return 1; }
/* remove the k-th element (0-based) if present */
static int remove_kth(Pool *p, int l, int k, int *v) {
    int16_t c = p->head[l];
    while (c != NIL && k--) c = p->cell[c].next;
    if (c == NIL) return 0;
    *v = p->cell[c].val; unlink_cell(p, l, c); return 1;
}

/* Compact: renumber live cells so the pool prefix holds them list by list. Returns new used count. */
static void compact(Pool *p) {
    Cell nc[POOL]; int16_t nh[LISTS], nt[LISTS]; int n = 0;
    for (int l = 0; l < LISTS; l++) {
        nh[l] = nt[l] = NIL;
        for (int16_t c = p->head[l]; c != NIL; c = p->cell[c].next) {
            nc[n].val = p->cell[c].val; nc[n].next = NIL; nc[n].prev = nt[l];
            if (nt[l] != NIL) nc[nt[l]].next = (int16_t)n; else nh[l] = (int16_t)n;
            nt[l] = (int16_t)n; n++;
        }
    }
    CHECK(n == p->used);
    for (int i = n; i < POOL; i++) { nc[i].val = 0; nc[i].prev = NIL; nc[i].next = (int16_t)(i + 1 < POOL ? i + 1 : -1); }
    memcpy(p->cell, nc, sizeof nc); memcpy(p->head, nh, sizeof nh); memcpy(p->tail, nt, sizeof nt);
    p->free_head = n < POOL ? (int16_t)n : NIL;
}

int main(void) {
    static Pool p; pool_init(&p);
    static int m[LISTS][POOL]; int mn[LISTS] = {0};
    long ops[6] = {0}, rejected = 0, compactions = 0;
    for (int step = 0; step < 44000; step++) {
        int l = (int)(rnd() % LISTS);
        unsigned op = rnd() % 100;
        int total = 0; for (int i = 0; i < LISTS; i++) total += mn[i];
        int v = (int)(rnd() % 100000);
        int phase = (step / 4000) % 2;
        if (phase == 0 && op >= 70) op %= 70;         /* growth phase: mostly adds */
        if (phase == 1 && op < 60) op = 60 + op % 40; /* drain phase: mostly removes */
        if (op < 20) { int ok = push_back(&p, l, v); CHECK(ok == (total < POOL)); if (ok) m[l][mn[l]++] = v; else rejected++; ops[0]++; }
        else if (op < 40) { int ok = push_front(&p, l, v); CHECK(ok == (total < POOL)); if (ok) { memmove(m[l] + 1, m[l], (size_t)mn[l] * sizeof(int)); m[l][0] = v; mn[l]++; } else rejected++; ops[1]++; }
        else if (op < 60) { /* move one element between lists */
            int to = (l + 1) % LISTS, x;
            if (mn[l]) { CHECK(pop_back(&p, l, &x) && x == m[l][mn[l] - 1]); mn[l]--; CHECK(push_back(&p, to, x)); m[to][mn[to]++] = x; }
            ops[2]++;
        } else if (op < 75) { int x; int ok = pop_front(&p, l, &x); CHECK(ok == (mn[l] > 0)); if (ok) { CHECK(x == m[l][0]); memmove(m[l], m[l] + 1, (size_t)(--mn[l]) * sizeof(int)); } ops[3]++; }
        else if (op < 90) { int x; int ok = pop_back(&p, l, &x); CHECK(ok == (mn[l] > 0)); if (ok) CHECK(x == m[l][--mn[l]]); ops[4]++; }
        else if (op < 99) {
            int k = (int)(rnd() % 12), x; int ok = remove_kth(&p, l, k, &x); CHECK(ok == (k < mn[l]));
            if (ok) { CHECK(x == m[l][k]); memmove(m[l] + k, m[l] + k + 1, (size_t)(mn[l] - k - 1) * sizeof(int)); mn[l]--; } ops[5]++;
        } else { compact(&p); compactions++; }
        for (int i = 0; i < LISTS; i++) {
            int n = 0;
            for (int16_t c = p.head[i]; c != NIL; c = p.cell[c].next) { CHECK(n < mn[i] && p.cell[c].val == m[i][n]); n++; }
            CHECK(n == mn[i]);
            if (step % 97 == 0) { n = 0; for (int16_t c = p.tail[i]; c != NIL; c = p.cell[c].prev) n++; CHECK(n == mn[i]); }
        }
    }
    printf("push_back=%ld push_front=%ld move=%ld pop_front=%ld pop_back=%ld remove_kth=%ld\n", ops[0], ops[1], ops[2], ops[3], ops[4], ops[5]);
    printf("rejected_full=%ld compactions=%ld peak_used=%d used=%d cell_bytes=%zu\n", rejected, compactions, p.peak, p.used, sizeof(Cell));
    printf("list sizes:"); for (int i = 0; i < LISTS; i++) printf(" %d", mn[i]);
    printf("\n");
    return 0;
}
