/*
 * title: Real-time queue by Hood-Melville incremental rotation
 * topic: data_structures
 * covers: persistent queue, worst-case O(1), incremental reversal state machine, schedules
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 2718281828u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct L { int v; struct L *next; } L;

static L **pool;
static size_t pool_n, pool_cap;
static L *cons(int v, L *next) {
    L *n = malloc(sizeof *n);
    CHECK(n);
    n->v = v; n->next = next;
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 1024;
        pool = realloc(pool, pool_cap * sizeof *pool);
        CHECK(pool);
    }
    pool[pool_n++] = n;
    return n;
}

enum { IDLE, REVERSING, APPENDING, DONE };
typedef struct { int kind, ok; L *f, *f2, *r, *r2; } State;
typedef struct { int lenf; L *f; State st; int lenr; L *r; } Queue;

static long steps_this_op, max_steps;

static State exec1(State s) {
    steps_this_op++;
    if (s.kind == REVERSING) {
        if (s.f && s.r) {
            State n = { REVERSING, s.ok + 1, s.f->next, cons(s.f->v, s.f2), s.r->next, cons(s.r->v, s.r2) };
            return n;
        }
        if (!s.f && s.r && !s.r->next) {
            State n = { APPENDING, s.ok, NULL, s.f2, NULL, cons(s.r->v, s.r2) };
            return n;
        }
        CHECK(0);
    } else if (s.kind == APPENDING) {
        if (s.ok == 0) {
            State n = { DONE, 0, NULL, NULL, NULL, s.r2 };
            return n;
        }
        State n = { APPENDING, s.ok - 1, NULL, s.f2->next, NULL, cons(s.f2->v, s.r2) };
        return n;
    }
    return s;
}
static State invalidate(State s) {
    if (s.kind == REVERSING) { s.ok--; return s; }
    if (s.kind == APPENDING) {
        if (s.ok == 0) {
            State n = { DONE, 0, NULL, NULL, NULL, s.r2->next };
            return n;
        }
        s.ok--;
        return s;
    }
    return s;
}
static Queue exec2(Queue q) {
    State s = exec1(exec1(q.st));
    if (s.kind == DONE) {
        Queue n = { q.lenf, s.r2, { IDLE, 0, NULL, NULL, NULL, NULL }, q.lenr, q.r };
        return n;
    }
    q.st = s;
    return q;
}
static Queue check_q(Queue q) {
    if (q.lenr <= q.lenf) return exec2(q);
    State st = { REVERSING, 0, q.f, NULL, q.r, NULL };
    Queue n = { q.lenf + q.lenr, q.f, st, 0, NULL };
    return exec2(n);
}
static Queue q_empty(void) {
    Queue q = { 0, NULL, { IDLE, 0, NULL, NULL, NULL, NULL }, 0, NULL };
    return q;
}
static Queue q_snoc(Queue q, int x) {
    steps_this_op = 0;
    q.lenr++;
    q.r = cons(x, q.r);
    Queue n = check_q(q);
    if (steps_this_op > max_steps) max_steps = steps_this_op;
    return n;
}
static int q_head(Queue q) { CHECK(q.f); return q.f->v; }
static Queue q_tail(Queue q) {
    steps_this_op = 0;
    CHECK(q.f);
    Queue n = { q.lenf - 1, q.f->next, invalidate(q.st), q.lenr, q.r };
    n = check_q(n);
    if (steps_this_op > max_steps) max_steps = steps_this_op;
    return n;
}
static int q_size(Queue q) { return q.lenf + q.lenr; }

#define VERS 20
#define CAP 400

int main(void) {
    Queue qs[VERS];
    int model[VERS][CAP], mlen[VERS], mhead[VERS];
    for (int i = 0; i < VERS; i++) { qs[i] = q_empty(); mlen[i] = mhead[i] = 0; }
    long snocs = 0, tails = 0;
    int idle_seen = 0, rev_seen = 0, app_seen = 0;
    for (int step = 0; step < 8000; step++) {
        int src = (int)(rnd() % VERS);
        int dst = (int)(rnd() % VERS);
        unsigned op = rnd() % 10;
        int n = mlen[src] - mhead[src];
        int want_snoc = op < 5 || n == 0;
        if (want_snoc && mlen[src] >= CAP) continue;
        int val = (int)(rnd() % 100000);
        Queue nq = want_snoc ? q_snoc(qs[src], val) : q_tail(qs[src]);
        if (dst != src) {
            for (int k = 0; k < mlen[src]; k++) model[dst][k] = model[src][k];
            mlen[dst] = mlen[src];
            mhead[dst] = mhead[src];
        }
        if (want_snoc) {
            model[dst][mlen[dst]++] = val;
            snocs++;
        } else {
            mhead[dst]++;
            tails++;
        }
        qs[dst] = nq;
        switch (nq.st.kind) {
        case IDLE: idle_seen++; break;
        case REVERSING: rev_seen++; break;
        default: app_seen++; break;
        }
        CHECK(q_size(nq) == mlen[dst] - mhead[dst]);
        CHECK(nq.lenr <= nq.lenf);
        if (q_size(nq) > 0) CHECK(q_head(nq) == model[dst][mhead[dst]]);
    }
    long total = 0;
    unsigned long sum = 0;
    for (int i = 0; i < VERS; i++) {
        Queue q = qs[i];
        int k = mhead[i];
        while (q_size(q) > 0) {
            int h = q_head(q);
            CHECK(k < mlen[i] && h == model[i][k]);
            sum = sum * 131u + (unsigned)h;
            q = q_tail(q);
            k++;
            total++;
        }
        CHECK(k == mlen[i]);
    }
    printf("snoc=%ld tail=%ld\n", snocs, tails);
    printf("states after ops: idle=%d reversing=%d appending=%d\n", idle_seen, rev_seen, app_seen);
    printf("drained %ld elements, checksum %lu\n", total, sum % 1000000007ul);
    printf("worst-case incremental steps in one operation: %ld\n", max_steps);
    CHECK(max_steps <= 4);
    for (size_t i = 0; i < pool_n; i++) free(pool[i]);
    free(pool);
    return 0;
}
