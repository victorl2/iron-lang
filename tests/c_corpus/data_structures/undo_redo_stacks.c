/*
 * title: Undo and redo with command stacks
 * topic: data_structures
 * covers: undo stack, redo stack, command records, function pointers, redo invalidation, history cap
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x4D2C6DFC5AC42AEDULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 18); }

#define CELLS 8
typedef struct { long cell[CELLS]; } Doc;

typedef enum { C_SET, C_ADD, C_SWAP, C_FILL, C_COUNT } Kind;
static const char *kind_name[C_COUNT] = { "set", "add", "swap", "fill" };

typedef struct Cmd Cmd;
struct Cmd {
    Kind kind; int a, b; long arg;
    long saved[CELLS];         /* state saved by apply() for undo (only used cells) */
    void (*apply)(Cmd *, Doc *);
    void (*revert)(Cmd *, Doc *);
};
static void ap_set(Cmd *c, Doc *d) { c->saved[0] = d->cell[c->a]; d->cell[c->a] = c->arg; }
static void rv_set(Cmd *c, Doc *d) { d->cell[c->a] = c->saved[0]; }
static void ap_add(Cmd *c, Doc *d) { d->cell[c->a] += c->arg; }
static void rv_add(Cmd *c, Doc *d) { d->cell[c->a] -= c->arg; }
static void ap_swap(Cmd *c, Doc *d) { long t = d->cell[c->a]; d->cell[c->a] = d->cell[c->b]; d->cell[c->b] = t; }
static void ap_fill(Cmd *c, Doc *d) { memcpy(c->saved, d->cell, sizeof d->cell); for (int i = c->a; i <= c->b; i++) d->cell[i] = c->arg; }
static void rv_fill(Cmd *c, Doc *d) { memcpy(d->cell, c->saved, sizeof d->cell); }

typedef struct { Cmd *undo[64], *redo[64]; int nu, nr; long dropped_redo, evicted; } History;

static Cmd *make_cmd(void) {
    Cmd *c = calloc(1, sizeof *c); CHECK(c);
    c->kind = (Kind)(rnd() % C_COUNT);
    c->a = (int)(rnd() % CELLS); c->b = (int)(rnd() % CELLS); c->arg = (long)(rnd() % 100) - 50;
    if (c->kind == C_FILL && c->a > c->b) { int t = c->a; c->a = c->b; c->b = t; }
    switch (c->kind) {
    case C_SET: c->apply = ap_set; c->revert = rv_set; break;
    case C_ADD: c->apply = ap_add; c->revert = rv_add; break;
    case C_SWAP: c->apply = ap_swap; c->revert = ap_swap; break;
    default: c->apply = ap_fill; c->revert = rv_fill; break;
    }
    return c;
}
static void do_cmd(History *h, Doc *d, Cmd *c) {
    for (int i = 0; i < h->nr; i++) { free(h->redo[i]); h->dropped_redo++; }
    h->nr = 0;
    if (h->nu == 64) { free(h->undo[0]); memmove(h->undo, h->undo + 1, 63 * sizeof(Cmd *)); h->nu--; h->evicted++; }
    c->apply(c, d); h->undo[h->nu++] = c;
}
static int undo(History *h, Doc *d) { if (!h->nu) return 0; Cmd *c = h->undo[--h->nu]; c->revert(c, d); h->redo[h->nr++] = c; return 1; }
static int redo(History *h, Doc *d) { if (!h->nr) return 0; Cmd *c = h->redo[--h->nr]; c->apply(c, d); h->undo[h->nu++] = c; return 1; }
static void history_free(History *h) { for (int i = 0; i < h->nu; i++) free(h->undo[i]); for (int i = 0; i < h->nr; i++) free(h->redo[i]); h->nu = h->nr = 0; }

int main(void) {
    Doc d; memset(&d, 0, sizeof d);
    History h; memset(&h, 0, sizeof h);
    /* the model: list of document snapshots, with a pointer into it */
    static Doc snaps[100000]; int ns = 1, at = 0, base = 0; snaps[0] = d;
    long kind_count[C_COUNT] = {0}, undos = 0, redos = 0, failed_undo = 0, failed_redo = 0;
    for (int step = 0; step < 20000; step++) {
        unsigned op = rnd() % 100;
        if (op < 55) {
            Cmd *c = make_cmd(); kind_count[c->kind]++;
            do_cmd(&h, &d, c);
            ns = at + 1; snaps[ns++] = d; at++;
            if (at - base > 64) base = at - 64; /* history cap mirrors evictions */
        } else if (op < 80) {
            int ok = undo(&h, &d); CHECK(ok == (at > base));
            if (ok) { at--; undos++; } else failed_undo++;
        } else {
            int ok = redo(&h, &d); CHECK(ok == (at + 1 < ns));
            if (ok) { at++; redos++; } else failed_redo++;
        }
        CHECK(memcmp(&d, &snaps[at], sizeof d) == 0);
        CHECK(h.nu == at - base && h.nr == ns - 1 - at);
        if (ns > 99000) { /* rebase the model */ memmove(snaps, snaps + base, (size_t)(ns - base) * sizeof(Doc)); ns -= base; at -= base; base = 0; }
    }
    printf("commands:"); for (int k = 0; k < C_COUNT; k++) printf(" %s=%ld", kind_name[k], kind_count[k]);
    printf("\nundo=%ld redo=%ld failed_undo=%ld failed_redo=%ld\n", undos, redos, failed_undo, failed_redo);
    printf("undo_depth=%d redo_depth=%d dropped_redo=%ld evicted=%ld\n", h.nu, h.nr, h.dropped_redo, h.evicted);
    printf("doc:"); for (int i = 0; i < CELLS; i++) printf(" %ld", d.cell[i]);
    printf("\n");
    /* unwind everything, the document must equal the oldest retained snapshot */
    while (undo(&h, &d)) {}
    CHECK(memcmp(&d, &snaps[base], sizeof d) == 0);
    printf("oldest retained:"); for (int i = 0; i < CELLS; i++) printf(" %ld", d.cell[i]);
    printf("\n");
    history_free(&h);
    return 0;
}
