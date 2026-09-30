/*
 * title: Unique owner with moved-from checks
 * topic: memory
 * covers: unique ownership, move-only handle, swap/reset/release, use-after-move detection, transfer through containers
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int id; char name[12]; long value; } Widget;

typedef struct {
    Widget *p;
    int moved; /* set on a moved-from Unique to catch use after move */
} Unique;

static int live;
static int misuse_caught;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Unique u_make(int id, const char *name, long v) {
    Widget *w = malloc(sizeof *w);
    check(w != NULL, "alloc");
    w->id = id;
    snprintf(w->name, sizeof w->name, "%s", name);
    w->value = v;
    live++;
    Unique u = {w, 0};
    return u;
}

static Widget *u_get(const Unique *u) {
    if (u->moved) {
        misuse_caught++;
        return NULL;
    }
    return u->p;
}

static Unique u_move(Unique *src) {
    Unique dst = *src;
    src->p = NULL;
    src->moved = 1;
    return dst;
}

static void u_reset(Unique *u) {
    if (u->p) {
        free(u->p);
        live--;
    }
    u->p = NULL;
    u->moved = 0;
}

static Widget *u_release(Unique *u) { /* give up ownership, caller must free */
    Widget *w = u->p;
    u->p = NULL;
    u->moved = 1;
    return w;
}

static void u_swap(Unique *a, Unique *b) {
    Unique t = *a;
    *a = *b;
    *b = t;
}

/* Sink: takes ownership of its argument. */
static long sink(Unique u) {
    Widget *w = u_get(&u);
    long v = w ? w->value * 2 : -1;
    u_reset(&u);
    return v;
}

/* Fixed container of owners: insertion moves, removal moves out. */
typedef struct { Unique slot[4]; } Shelf;

static int shelf_put(Shelf *s, Unique *u) {
    for (int i = 0; i < 4; i++)
        if (!s->slot[i].p) {
            s->slot[i] = u_move(u);
            return i;
        }
    return -1;
}

static Unique shelf_take(Shelf *s, int i) { return u_move(&s->slot[i]); }

int main(void) {
    Unique a = u_make(1, "gear", 10);
    Unique b = u_make(2, "cog", 20);
    printf("a=%s b=%s live=%d\n", u_get(&a)->name, u_get(&b)->name, live);

    u_swap(&a, &b);
    printf("swapped: a=%s b=%s\n", u_get(&a)->name, u_get(&b)->name);

    printf("sink(a) -> %ld\n", sink(u_move(&a)));
    printf("live after sink: %d\n", live);
    printf("use after move: %s\n", u_get(&a) ? "reads" : "detected");

    Shelf shelf;
    memset(&shelf, 0, sizeof shelf);
    Unique ws[3] = {u_make(3, "x", 1), u_make(4, "y", 2), u_make(5, "z", 3)};
    for (int i = 0; i < 3; i++)
        printf("put %d at slot %d\n", i, shelf_put(&shelf, &ws[i]));
    for (int i = 0; i < 3; i++)
        check(ws[i].moved && !ws[i].p, "moved");
    Unique t = shelf_take(&shelf, 1);
    printf("took %s (id %d), slot1 now %s\n", u_get(&t)->name, u_get(&t)->id,
           shelf.slot[1].moved ? "moved-out" : "full");
    int slot = shelf_put(&shelf, &t);
    printf("reput at %d\n", slot);

    Widget *raw = u_release(&b);
    printf("released %s, unique now %s\n", raw->name, u_get(&b) ? "valid" : "empty");
    free(raw);
    live--;

    long total = 0;
    for (int i = 0; i < 4; i++) {
        Widget *w = u_get(&shelf.slot[i]);
        if (w)
            total += w->value;
        u_reset(&shelf.slot[i]);
    }
    printf("shelf total %ld, misuse caught %d, live %d\n", total, misuse_caught, live);
    check(live == 0, "leak");
    check(misuse_caught == 2, "misuse count");
    return 0;
}
