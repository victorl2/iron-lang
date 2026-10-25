/*
 * title: Sweep line detection of any segment intersection
 * topic: algorithms
 * covers: sweep line, Shamos-Hoey, event ordering, ordered active set, exact rational comparison
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long long x, y;
} Pt;

typedef struct {
    Pt p, q; /* p.x < q.x, or p.x == q.x and p.y <= q.y */
} Seg;

typedef struct {
    long long x;
    int type; /* +1 insert, -1 remove */
    int id;
} Event;

static unsigned s = 20240607u;

static unsigned rnd(void) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int sgn(long long v) {
    return (v > 0) - (v < 0);
}

static int orient(Pt a, Pt b, Pt c) {
    return sgn((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

static long long mn(long long a, long long b) {
    return a < b ? a : b;
}

static long long mx(long long a, long long b) {
    return a > b ? a : b;
}

static int intersect(const Seg *a, const Seg *b) {
    if (mx(a->p.x, b->p.x) > mn(a->q.x, b->q.x))
        return 0;
    if (mx(mn(a->p.y, a->q.y), mn(b->p.y, b->q.y)) > mn(mx(a->p.y, a->q.y), mx(b->p.y, b->q.y)))
        return 0;
    return orient(a->p, a->q, b->p) * orient(a->p, a->q, b->q) <= 0 &&
           orient(b->p, b->q, a->p) * orient(b->p, b->q, a->q) <= 0;
}

/* y of segment at x as a fraction num/den, den > 0 (vertical segments use their low end). */
static void y_at(const Seg *g, long long x, long long *num, long long *den) {
    long long dx = g->q.x - g->p.x;
    if (dx == 0) {
        *num = g->p.y;
        *den = 1;
        return;
    }
    *den = dx;
    *num = g->p.y * dx + (g->q.y - g->p.y) * (x - g->p.x);
}

/* strict "a below b" at the larger of the two left ends */
static int below(const Seg *a, const Seg *b) {
    long long x = mx(a->p.x, b->p.x);
    long long na, da, nb, db;
    y_at(a, x, &na, &da);
    y_at(b, x, &nb, &db);
    return na * db < nb * da;
}

static int cmp_event(const void *pa, const void *pb) {
    const Event *a = pa, *b = pb;
    if (a->x != b->x)
        return a->x < b->x ? -1 : 1;
    if (a->type != b->type)
        return a->type > b->type ? -1 : 1; /* inserts before removals so touching ends are seen */
    return a->id - b->id;
}

/* Returns 1 and the offending pair if any two segments meet. */
static int sweep_any(const Seg *sg, int n, int *ra, int *rb, long *ops) {
    Event *ev = malloc(sizeof(Event) * (size_t)(2 * n));
    int *act = malloc(sizeof(int) * (size_t)n);
    int na = 0, found = 0;
    for (int i = 0; i < n; i++) {
        ev[2 * i] = (Event){sg[i].p.x, +1, i};
        ev[2 * i + 1] = (Event){sg[i].q.x, -1, i};
    }
    qsort(ev, (size_t)(2 * n), sizeof(Event), cmp_event);
    for (int e = 0; e < 2 * n && !found; e++) {
        int id = ev[e].id;
        if (ev[e].type > 0) {
            int pos = 0;
            while (pos < na && below(&sg[act[pos]], &sg[id])) {
                pos++;
                (*ops)++;
            }
            if (pos < na && intersect(&sg[act[pos]], &sg[id])) {
                *ra = act[pos];
                *rb = id;
                found = 1;
            } else if (pos > 0 && intersect(&sg[act[pos - 1]], &sg[id])) {
                *ra = act[pos - 1];
                *rb = id;
                found = 1;
            }
            memmove(act + pos + 1, act + pos, sizeof(int) * (size_t)(na - pos));
            act[pos] = id;
            na++;
        } else {
            int pos = 0;
            while (act[pos] != id)
                pos++;
            if (pos > 0 && pos + 1 < na && intersect(&sg[act[pos - 1]], &sg[act[pos + 1]])) {
                *ra = act[pos - 1];
                *rb = act[pos + 1];
                found = 1;
            }
            memmove(act + pos, act + pos + 1, sizeof(int) * (size_t)(na - pos - 1));
            na--;
        }
    }
    free(ev);
    free(act);
    return found;
}

int main(void) {
    int trials = 300, hits = 0, misses = 0;
    long ops = 0;
    unsigned checksum = 0;
    for (int t = 0; t < trials; t++) {
        int n = 2 + (int)(rnd() % 14);
        int reach = 1 + (int)(rnd() % 4); /* short segments give sparse, mostly disjoint sets */
        Seg sg[16];
        for (int i = 0; i < n; i++) {
            Pt a = {(long long)(rnd() % 40), (long long)(rnd() % 40)};
            Pt b = {a.x + (long long)(rnd() % (unsigned)(reach * 3)), a.y + (long long)(rnd() % (unsigned)(reach * 3)) - reach};
            if (rnd() & 1)
                b.y = a.y + (long long)(rnd() % (unsigned)(reach * 3)) - reach;
            if (a.x == b.x && a.y == b.y)
                b.x++;
            if (a.x > b.x || (a.x == b.x && a.y > b.y)) {
                Pt tmp = a;
                a = b;
                b = tmp;
            }
            sg[i] = (Seg){a, b};
        }
        int ra = -1, rb = -1;
        int got = sweep_any(sg, n, &ra, &rb, &ops);
        int want = 0;
        for (int i = 0; i < n && !want; i++)
            for (int j = i + 1; j < n; j++)
                if (intersect(&sg[i], &sg[j])) {
                    want = 1;
                    break;
                }
        check(got == want, "sweep agrees with brute force");
        if (got)
            check(intersect(&sg[ra], &sg[rb]), "reported pair really intersects");
        hits += got;
        misses += !got;
        checksum = checksum * 31u + (unsigned)got + (unsigned)(n * 3);
    }
    printf("trials=%d with_intersection=%d disjoint_sets=%d\n", trials, hits, misses);
    printf("active-set scan steps=%ld checksum=%u\n", ops, checksum);
    /* fixed scenes */
    Seg a[] = {{{0, 0}, {10, 0}}, {{0, 2}, {10, 2}}, {{0, 4}, {10, 4}}};
    int ra, rb;
    long o = 0;
    printf("parallel stack: %d\n", sweep_any(a, 3, &ra, &rb, &o));
    Seg b[] = {{{0, 0}, {10, 10}}, {{0, 10}, {10, 0}}};
    printf("cross: %d\n", sweep_any(b, 2, &ra, &rb, &o));
    Seg c[] = {{{0, 0}, {4, 0}}, {{4, 0}, {8, 3}}};
    printf("endpoint touch: %d\n", sweep_any(c, 2, &ra, &rb, &o));
    Seg d[] = {{{0, 0}, {10, 0}}, {{5, -3}, {5, 3}}};
    printf("vertical crossing: %d\n", sweep_any(d, 2, &ra, &rb, &o));
    return 0;
}
