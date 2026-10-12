/*
 * title: Cycle collection by trial deletion
 * topic: memory
 * covers: refcounting with cycle collector, Bacon-Rajan trial deletion, mark gray/scan/collect white, suspected roots
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Color { BLACK, GRAY, WHITE, DEAD };

typedef struct Obj Obj;
struct Obj {
    int rc;
    int trial;
    enum Color color;
    int buffered;
    int id;
    Obj *out[3];
    int nout;
    int freed;
};

#define MAXOBJ 32
static Obj *all[MAXOBJ];
static int nall;
static Obj *roots[MAXOBJ];
static int nroots;
static int live;
static int collected_order[MAXOBJ];
static int ncollected;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Obj *obj_new(int id) {
    Obj *o = calloc(1, sizeof *o);
    check(o != NULL, "alloc");
    o->rc = 1; /* creator's reference */
    o->id = id;
    all[nall++] = o;
    live++;
    return o;
}

static void link_to(Obj *from, Obj *to) {
    from->out[from->nout++] = to;
    to->rc++;
}

static void add_root(Obj *o) {
    if (!o->buffered) {
        o->buffered = 1;
        roots[nroots++] = o;
    }
}

static void obj_free(Obj *o) {
    o->freed = 1;
    live--;
}

/* release a reference held by an external variable */
static void release(Obj *o) {
    if (--o->rc == 0) {
        for (int i = 0; i < o->nout; i++)
            release(o->out[i]);
        o->nout = 0;
        obj_free(o);
    } else {
        add_root(o); /* possible cycle root */
    }
}

static void mark_gray(Obj *o) {
    if (o->color == GRAY)
        return;
    o->color = GRAY;
    o->trial = o->rc;
    for (int i = 0; i < o->nout; i++)
        mark_gray(o->out[i]);
}

static void scan_black(Obj *o) {
    o->color = BLACK;
    for (int i = 0; i < o->nout; i++)
        if (o->out[i]->color != BLACK)
            scan_black(o->out[i]);
}

static void scan(Obj *o) {
    if (o->color != GRAY)
        return;
    if (o->trial > 0) {
        scan_black(o);
    } else {
        o->color = WHITE;
        for (int i = 0; i < o->nout; i++)
            scan(o->out[i]);
    }
}

static void collect_white(Obj *o) {
    if (o->color != WHITE || o->freed)
        return;
    o->color = DEAD;
    collected_order[ncollected++] = o->id;
    for (int i = 0; i < o->nout; i++)
        collect_white(o->out[i]);
}

static void collect_cycles(void) {
    for (int i = 0; i < nroots; i++)
        if (!roots[i]->freed)
            mark_gray(roots[i]);
    /* compute trial counts: subtract internal edges once per edge */
    for (int i = 0; i < nall; i++)
        if (all[i]->color == GRAY && !all[i]->freed)
            all[i]->trial = all[i]->rc;
    for (int i = 0; i < nall; i++)
        if (all[i]->color == GRAY && !all[i]->freed)
            for (int k = 0; k < all[i]->nout; k++)
                if (all[i]->out[k]->color == GRAY)
                    all[i]->out[k]->trial--;
    for (int i = 0; i < nroots; i++)
        if (!roots[i]->freed)
            scan(roots[i]);
    for (int i = 0; i < nroots; i++) {
        Obj *r = roots[i];
        r->buffered = 0;
        if (!r->freed)
            collect_white(r);
    }
    nroots = 0;
    /* dead objects give back their references to survivors, then die */
    for (int i = 0; i < nall; i++)
        if (all[i]->color == DEAD && !all[i]->freed)
            for (int k = 0; k < all[i]->nout; k++)
                if (all[i]->out[k]->color != DEAD)
                    all[i]->out[k]->rc--;
    for (int i = 0; i < nall; i++)
        if (all[i]->color == DEAD && !all[i]->freed)
            obj_free(all[i]);
    for (int i = 0; i < nall; i++)
        if (!all[i]->freed)
            all[i]->color = BLACK;
}

int main(void) {
    /* cycle a -> b -> c -> a, plus tail c -> d; d also referenced externally */
    Obj *a = obj_new(1), *b = obj_new(2), *c = obj_new(3), *d = obj_new(4);
    link_to(a, b);
    link_to(b, c);
    link_to(c, a);
    link_to(c, d);
    release(b);
    release(c);
    release(a); /* external refs on the cycle are gone: a,b,c are garbage */
    printf("live before collect: %d, roots buffered: %d\n", live, nroots);
    collect_cycles();
    printf("collected order:");
    for (int i = 0; i < ncollected; i++)
        printf(" %d", collected_order[i]);
    printf("\nlive after collect: %d (d survives: rc=%d)\n", live, d->rc);
    check(!d->freed && d->rc == 1, "d kept alive by external ref");

    /* self-loop and a live cycle that must NOT be collected */
    Obj *e = obj_new(5), *f = obj_new(6), *g = obj_new(7);
    link_to(e, e);
    link_to(f, g);
    link_to(g, f);
    release(e);  /* e: only self ref left -> garbage */
    release(g);  /* g still reachable from f's external ref */
    ncollected = 0;
    collect_cycles();
    printf("second collect order:");
    for (int i = 0; i < ncollected; i++)
        printf(" %d", collected_order[i]);
    printf("\nlive: %d, f alive=%d g alive=%d\n", live, !f->freed, !g->freed);
    check(e->freed && !f->freed && !g->freed && ncollected == 1, "live cycle preserved");

    release(f); /* now the f<->g cycle is garbage too */
    ncollected = 0;
    collect_cycles();
    printf("third collect count: %d, live: %d\n", ncollected, live);
    release(d);
    printf("final live: %d\n", live);
    check(live == 0, "all reclaimed");
    for (int i = 0; i < nall; i++)
        free(all[i]);
    return 0;
}
