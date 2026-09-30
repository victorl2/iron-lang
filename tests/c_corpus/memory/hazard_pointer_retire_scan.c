/*
 * title: Hazard pointers with retire lists (simulated)
 * topic: memory
 * covers: hazard pointer slots, protect/validate loop, retire list scan threshold, safe reclamation, interleaved actors
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NACTORS 3
#define SCAN_THRESHOLD 4

typedef struct Cell Cell;
struct Cell {
    int value;
    int freed_marker; /* set to a magic value before free to detect stale reads */
};

static Cell *shared_ptr; /* the single published cell */
static Cell *hazard[NACTORS];
static Cell *retired[NACTORS][16];
static int nretired[NACTORS];
static int live_cells, total_freed, stale_reads;
static int scans;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Cell *cell_new(int v) {
    Cell *c = malloc(sizeof *c);
    check(c != NULL, "alloc");
    c->value = v;
    c->freed_marker = 0;
    live_cells++;
    return c;
}

static int is_hazardous(Cell *c) {
    for (int i = 0; i < NACTORS; i++)
        if (hazard[i] == c)
            return 1;
    return 0;
}

static void scan(int actor) {
    scans++;
    int w = 0;
    for (int i = 0; i < nretired[actor]; i++) {
        Cell *c = retired[actor][i];
        if (is_hazardous(c)) {
            retired[actor][w++] = c;
        } else {
            c->freed_marker = 0xDEAD;
            free(c);
            live_cells--;
            total_freed++;
        }
    }
    nretired[actor] = w;
}

static void retire(int actor, Cell *c) {
    check(nretired[actor] < 16, "retire list overflow");
    retired[actor][nretired[actor]++] = c;
    if (nretired[actor] >= SCAN_THRESHOLD)
        scan(actor);
}

/* Reader protocol: publish hazard, re-check the source still matches. */
static Cell *protect(int actor) {
    for (;;) {
        Cell *c = shared_ptr;
        hazard[actor] = c;
        if (shared_ptr == c) /* would be a seq_cst reload in the concurrent version */
            return c;
    }
}

static void clear(int actor) { hazard[actor] = NULL; }

/* Writer replaces the published cell and retires the old one. */
static void update(int actor, int v) {
    Cell *n = cell_new(v);
    Cell *old = shared_ptr;
    shared_ptr = n;
    retire(actor, old);
}

int main(void) {
    shared_ptr = cell_new(0);
    /* Actor 2 is a slow reader that protects a cell and keeps it for many steps. */
    Cell *slow = protect(2);
    printf("slow reader holds value %d\n", slow->value);

    unsigned s = 12345u;
    int steps = 30;
    int held_ok = 1;
    for (int step = 1; step <= steps; step++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int actor = (int)(s % 2); /* actors 0,1 write */
        update(actor, step);
        /* fast reader (actor 0 also reads) */
        Cell *c = protect(0);
        if (c->freed_marker == 0xDEAD)
            stale_reads++;
        clear(0);
        if (slow->freed_marker == 0xDEAD)
            held_ok = 0;
        if (step % 10 == 0)
            printf("step %2d: live=%d freed=%d retired=[%d,%d] scans=%d\n", step, live_cells,
                   total_freed, nretired[0], nretired[1], scans);
    }
    check(held_ok, "slow reader's cell was reclaimed");
    printf("slow reader still reads %d, freed marker %d\n", slow->value, slow->freed_marker);
    printf("stale reads: %d\n", stale_reads);

    clear(2);
    for (int a = 0; a < 2; a++)
        scan(a);
    printf("after releasing slow reader: live=%d (only the published cell), retired=[%d,%d]\n",
           live_cells, nretired[0], nretired[1]);
    check(live_cells == 1 && nretired[0] == 0 && nretired[1] == 0, "drained");
    printf("published value %d\n", shared_ptr->value);
    free(shared_ptr);
    live_cells--;
    check(live_cells == 0, "leak");
    return 0;
}
