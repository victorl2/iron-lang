/*
 * title: Runtime borrow checker (RefCell style)
 * topic: memory
 * covers: shared/exclusive borrow counting, borrow guards, conflict detection, guard release ordering
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int value;
    int shared; /* number of live shared borrows */
    int exclusive; /* 0 or 1 */
    int conflicts;
    int max_shared;
} Cell;

typedef struct {
    Cell *c;
    int kind; /* 0 none (failed), 1 shared, 2 exclusive */
} Guard;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Guard borrow(Cell *c) {
    Guard g = {c, 0};
    if (c->exclusive) {
        c->conflicts++;
        return g;
    }
    c->shared++;
    if (c->shared > c->max_shared)
        c->max_shared = c->shared;
    g.kind = 1;
    return g;
}

static Guard borrow_mut(Cell *c) {
    Guard g = {c, 0};
    if (c->exclusive || c->shared) {
        c->conflicts++;
        return g;
    }
    c->exclusive = 1;
    g.kind = 2;
    return g;
}

static void release(Guard *g) {
    if (g->kind == 1) {
        check(g->c->shared > 0, "shared underflow");
        g->c->shared--;
    } else if (g->kind == 2) {
        check(g->c->exclusive == 1, "exclusive release");
        g->c->exclusive = 0;
    }
    g->kind = 0;
}

static const char *kn(Guard g) {
    return g.kind == 0 ? "DENIED" : g.kind == 1 ? "shared" : "exclusive";
}

/* A function that takes a mutable borrow and calls back, like a visitor. */
static int visit(Cell *c, int (*fn)(Cell *)) {
    Guard g = borrow_mut(c);
    if (!g.kind)
        return -1;
    c->value += 1;
    int r = fn(c);
    release(&g);
    return r;
}

static int reentrant(Cell *c) {
    Guard g = borrow(c); /* conflicts with the exclusive borrow held by visit() */
    int r = g.kind ? c->value : -99;
    release(&g);
    return r;
}

static int benign(Cell *c) { return c->value * 10; }

int main(void) {
    Cell c = {5, 0, 0, 0, 0};
    Guard r1 = borrow(&c), r2 = borrow(&c), r3 = borrow(&c);
    printf("readers: %s %s %s (shared=%d)\n", kn(r1), kn(r2), kn(r3), c.shared);
    Guard w = borrow_mut(&c);
    printf("writer while readers: %s\n", kn(w));
    release(&r1);
    release(&r2);
    w = borrow_mut(&c);
    printf("writer with 1 reader left: %s\n", kn(w));
    release(&r3);
    w = borrow_mut(&c);
    printf("writer with no readers: %s\n", kn(w));
    Guard r4 = borrow(&c);
    printf("reader while writing: %s\n", kn(r4));
    c.value = 42;
    release(&w);
    r4 = borrow(&c);
    printf("reader after write: %s value=%d\n", kn(r4), c.value);
    release(&r4);

    int vr = visit(&c, reentrant);
    printf("visit reentrant: %d (conflicts=%d)\n", vr, c.conflicts);
    vr = visit(&c, benign);
    printf("visit benign: %d\n", vr);

    /* nested scopes: guards stack up and unwind in reverse order */
    Guard stack[6];
    int n = 0;
    for (int i = 0; i < 6; i++) {
        stack[n] = (i % 3 == 2) ? borrow_mut(&c) : borrow(&c);
        printf("%s%s", kn(stack[n]), i < 5 ? " " : "\n");
        n++;
    }
    while (n > 0)
        release(&stack[--n]);
    check(c.shared == 0 && c.exclusive == 0, "all released");
    printf("max concurrent shared: %d, total conflicts: %d, final value %d\n", c.max_shared,
           c.conflicts, c.value);
    return 0;
}
