/*
 * title: Banker's queue with memoized lazy rotation
 * topic: data_structures
 * covers: persistent queue, lazy suspensions, memoization, amortized analysis under persistence
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 88172645u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Cell Cell;
typedef struct Susp Susp;
typedef struct RL RL;
struct Cell { int v; Susp *tl; };
struct Susp { int forced; Cell *val; Susp *f; RL *r; }; /* unforced: f ++ reverse r */
struct RL { int v; RL *next; };

static void **arena;
static size_t arena_n, arena_cap;

static void *xalloc(size_t sz) {
    void *p = malloc(sz);
    CHECK(p);
    if (arena_n == arena_cap) {
        arena_cap = arena_cap ? arena_cap * 2 : 1024;
        arena = realloc(arena, arena_cap * sizeof *arena);
        CHECK(arena);
    }
    arena[arena_n++] = p;
    return p;
}

static long force_steps, reverse_steps;

static Susp *susp_value(Cell *c) {
    Susp *s = xalloc(sizeof *s);
    s->forced = 1; s->val = c; s->f = NULL; s->r = NULL;
    return s;
}
static Susp *susp_append(Susp *f, RL *r) {
    Susp *s = xalloc(sizeof *s);
    s->forced = 0; s->val = NULL; s->f = f; s->r = r;
    return s;
}
static Cell *force(Susp *s) {
    if (s->forced) return s->val;
    force_steps++;
    Cell *fc = force(s->f);
    if (fc) {
        Cell *c = xalloc(sizeof *c);
        c->v = fc->v;
        c->tl = susp_append(fc->tl, s->r);
        s->val = c;
    } else {
        Susp *tail = susp_value(NULL);
        for (RL *r = s->r; r; r = r->next) {
            Cell *c = xalloc(sizeof *c);
            c->v = r->v;
            c->tl = tail;
            tail = susp_value(c);
            reverse_steps++;
        }
        s->val = tail->val;
    }
    s->forced = 1;
    return s->val;
}

typedef struct { Susp *f; int lenf; RL *r; int lenr; } Queue;

static Queue q_empty(void) {
    Queue q = { susp_value(NULL), 0, NULL, 0 };
    return q;
}
static Queue check_q(Queue q) {
    if (q.lenr <= q.lenf) return q;
    Queue n = { susp_append(q.f, q.r), q.lenf + q.lenr, NULL, 0 };
    return n;
}
static Queue q_snoc(Queue q, int x) {
    RL *r = xalloc(sizeof *r);
    r->v = x; r->next = q.r;
    Queue n = { q.f, q.lenf, r, q.lenr + 1 };
    return check_q(n);
}
static int q_head(Queue q) {
    Cell *c = force(q.f);
    CHECK(c);
    return c->v;
}
static Queue q_tail(Queue q) {
    Cell *c = force(q.f);
    CHECK(c);
    Queue n = { c->tl, q.lenf - 1, q.r, q.lenr };
    return check_q(n);
}
static int q_size(Queue q) { return q.lenf + q.lenr; }

#define VERS 24
#define CAP 512

int main(void) {
    Queue qs[VERS];
    int model[VERS][CAP];
    int mlen[VERS], mhead[VERS];
    for (int i = 0; i < VERS; i++) { qs[i] = q_empty(); mlen[i] = 0; mhead[i] = 0; }
    long snocs = 0, tails = 0, heads = 0;
    for (int step = 0; step < 6000; step++) {
        int src = (int)(rnd() % VERS);
        int dst = (int)(rnd() % VERS);
        unsigned op = rnd() % 8;
        int n = mlen[src] - mhead[src];
        if (op < 4 && mlen[src] < CAP) {
            int v = (int)(rnd() % 10000);
            qs[dst] = q_snoc(qs[src], v);
            if (dst != src) {
                for (int k = 0; k < mlen[src]; k++) model[dst][k] = model[src][k];
                mlen[dst] = mlen[src];
                mhead[dst] = mhead[src];
            }
            model[dst][mlen[dst]++] = v;
            snocs++;
        } else if (op < 6 && n > 0) {
            qs[dst] = q_tail(qs[src]);
            if (dst != src) {
                for (int k = 0; k < mlen[src]; k++) model[dst][k] = model[src][k];
                mlen[dst] = mlen[src];
                mhead[dst] = mhead[src];
            }
            mhead[dst]++;
            tails++;
        } else if (n > 0) {
            CHECK(q_head(qs[src]) == model[src][mhead[src]]);
            heads++;
        }
        CHECK(q_size(qs[dst]) == mlen[dst] - mhead[dst]);
        CHECK(qs[dst].lenr <= qs[dst].lenf);
    }
    /* drain every version and compare against the model */
    long total = 0;
    unsigned long sum = 0;
    for (int i = 0; i < VERS; i++) {
        Queue q = qs[i];
        int k = mhead[i];
        while (q_size(q) > 0) {
            int h = q_head(q);
            CHECK(k < mlen[i] && h == model[i][k]);
            sum = sum * 31u + (unsigned)h;
            q = q_tail(q);
            k++;
            total++;
        }
        CHECK(k == mlen[i]);
    }
    printf("snoc=%ld tail=%ld head=%ld\n", snocs, tails, heads);
    printf("drained %ld elements across %d versions, checksum %lu\n", total, VERS, sum % 1000000007ul);
    printf("suspension forces=%ld, reverse steps=%ld\n", force_steps, reverse_steps);

    /* single-threaded use: total work stays linear in the number of operations */
    force_steps = reverse_steps = 0;
    Queue q = q_empty();
    for (int i = 0; i < 2000; i++) q = q_snoc(q, i);
    for (int i = 0; i < 2000; i++) {
        CHECK(q_head(q) == i);
        q = q_tail(q);
    }
    CHECK(q_size(q) == 0);
    printf("linear run: forces=%ld reverse steps=%ld\n", force_steps, reverse_steps);
    CHECK(reverse_steps <= 2 * 2000);
    for (size_t i = 0; i < arena_n; i++) free(arena[i]);
    free(arena);
    return 0;
}
