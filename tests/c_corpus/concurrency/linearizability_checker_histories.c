/*
 * title: Linearizability checker for small recorded histories
 * topic: concurrency
 * covers: Wing-Gong search, real-time order, sequential models (register, queue, counter), memoized DFS, recorded atomic histories
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A history is a list of operations with invocation and response timestamps. It is linearizable
 * if the operations can be ordered sequentially such that (1) the order respects real time (if
 * a.response < b.invoke then a comes before b) and (2) replaying the order on a sequential model
 * reproduces every recorded result. The checker searches: at each step it may linearize any
 * not-yet-placed operation that is minimal in real-time order (no other unplaced operation
 * finished before it started), if the model accepts its result. Failed (mask, state) pairs are
 * memoized.
 *
 * Part 1 checks hand-written histories with known answers. Part 2 records real concurrent runs of
 * an atomic register and an atomic counter (timestamps from a global atomic clock taken before
 * invoking and after returning), which must always be linearizable, and then corrupts one result
 * to a value that no execution could produce, which must always be rejected.
 */
enum { MAXOPS = 16, KIND_WRITE = 0, KIND_READ = 1, KIND_ENQ = 2, KIND_DEQ = 3, KIND_FAA = 4 };
enum { MODEL_REG = 0, MODEL_QUEUE = 1, MODEL_COUNTER = 2 };

typedef struct {
    int kind, arg, ret;
    long inv, res;
} Op;

typedef struct {
    int model;
    int n;
    Op op[MAXOPS];
} History;

typedef struct {
    int reg;      /* register value or counter */
    int q[MAXOPS];
    int qn;
} State;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* apply op to state; returns 1 if the recorded result is consistent */
static int apply(int model, State *s, const Op *o) {
    (void)model;
    switch (o->kind) {
    case KIND_WRITE: s->reg = o->arg; return 1;
    case KIND_READ: return s->reg == o->ret;
    case KIND_FAA:
        if (s->reg != o->ret)
            return 0;
        s->reg += o->arg;
        return 1;
    case KIND_ENQ: s->q[s->qn++] = o->arg; return 1;
    case KIND_DEQ:
        if (s->qn == 0)
            return o->ret == -1;
        if (o->ret != s->q[0])
            return 0;
        memmove(s->q, s->q + 1, (size_t)(s->qn - 1) * sizeof(int));
        s->qn--;
        return 1;
    }
    return 0;
}

#define MEMO 8192
static uint64_t memo[MEMO];
static long nodes_visited;

static uint64_t state_hash(unsigned mask, const State *s) {
    uint64_t h = 1469598103934665603ull ^ mask;
    h *= 1099511628211ull;
    h = (h ^ (uint64_t)(unsigned)s->reg) * 1099511628211ull;
    h = (h ^ (uint64_t)s->qn) * 1099511628211ull;
    for (int i = 0; i < s->qn; i++)
        h = (h ^ (uint64_t)(unsigned)s->q[i]) * 1099511628211ull;
    return h | 1u; /* never 0 */
}

static int memo_has(uint64_t h) {
    for (unsigned i = (unsigned)h % MEMO;; i = (i + 1) % MEMO) {
        if (memo[i] == 0)
            return 0;
        if (memo[i] == h)
            return 1;
    }
}
static void memo_add(uint64_t h) {
    unsigned i = (unsigned)h % MEMO;
    while (memo[i] != 0 && memo[i] != h)
        i = (i + 1) % MEMO;
    memo[i] = h;
}

static int search(const History *h, unsigned done, const State *s) {
    nodes_visited++;
    if (done == (1u << h->n) - 1)
        return 1;
    uint64_t key = state_hash(done, s);
    if (memo_has(key))
        return 0;
    for (int i = 0; i < h->n; i++) {
        if (done & (1u << i))
            continue;
        int minimal = 1;
        for (int j = 0; j < h->n; j++)
            if (j != i && !(done & (1u << j)) && h->op[j].res < h->op[i].inv)
                minimal = 0;
        if (!minimal)
            continue;
        State t = *s;
        if (apply(h->model, &t, &h->op[i]) && search(h, done | (1u << i), &t))
            return 1;
    }
    memo_add(key);
    return 0;
}

static int linearizable(const History *h, State init) {
    memset(memo, 0, sizeof memo);
    nodes_visited = 0;
    return search(h, 0, &init);
}

static void add(History *h, int kind, int arg, int ret, long inv, long res) {
    Op *o = &h->op[h->n++];
    o->kind = kind;
    o->arg = arg;
    o->ret = ret;
    o->inv = inv;
    o->res = res;
}

/* ---- recorded concurrent executions ---- */
enum { RT = 3, RPER = 3 };
static atomic_long clk;
static atomic_int areg;
static atomic_int acnt;
static Op rec[RT][RPER];
static int rec_model;

static void *recorder(void *p) {
    int t = (int)(size_t)p;
    for (int i = 0; i < RPER; i++) {
        Op *o = &rec[t][i];
        int v = t * 10 + i + 1;
        o->inv = atomic_fetch_add(&clk, 1);
        if (rec_model == MODEL_REG) {
            if (i % 2 == 0) {
                o->kind = KIND_WRITE;
                o->arg = v;
                atomic_store(&areg, v);
                o->ret = 0;
            } else {
                o->kind = KIND_READ;
                o->arg = 0;
                o->ret = atomic_load(&areg);
            }
        } else {
            o->kind = KIND_FAA;
            o->arg = t + 1;
            o->ret = atomic_fetch_add(&acnt, t + 1);
        }
        o->res = atomic_fetch_add(&clk, 1);
    }
    return NULL;
}

static void build_recorded(History *h, int model) {
    rec_model = model;
    atomic_store(&clk, 0);
    atomic_store(&areg, 0);
    atomic_store(&acnt, 0);
    pthread_t th[RT];
    for (int t = 0; t < RT; t++)
        check(pthread_create(&th[t], NULL, recorder, (void *)(size_t)t) == 0, "create");
    for (int t = 0; t < RT; t++)
        pthread_join(th[t], NULL);
    h->model = model;
    h->n = 0;
    for (int t = 0; t < RT; t++)
        for (int i = 0; i < RPER; i++)
            h->op[h->n++] = rec[t][i];
}

int main(void) {
    State zero;
    memset(&zero, 0, sizeof zero);

    History h;
    /* register: a read overlapping a write may return old or new */
    h.model = MODEL_REG; h.n = 0;
    add(&h, KIND_WRITE, 5, 0, 0, 10);
    add(&h, KIND_READ, 0, 5, 2, 4);
    add(&h, KIND_READ, 0, 0, 3, 5);
    printf("register, overlapping reads of 0 and 5: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(linearizable(&h, zero), "reg overlap");

    h.n = 0;
    add(&h, KIND_WRITE, 5, 0, 0, 1);
    add(&h, KIND_READ, 0, 0, 2, 3);
    printf("register, read of 0 after a completed write of 5: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(!linearizable(&h, zero), "stale read rejected");

    h.n = 0;
    add(&h, KIND_WRITE, 1, 0, 0, 2);
    add(&h, KIND_WRITE, 2, 0, 3, 4);
    add(&h, KIND_READ, 0, 1, 5, 6);
    printf("register, read of 1 after write 1 then write 2: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(!linearizable(&h, zero), "old value after newer write");

    /* FIFO queue */
    h.model = MODEL_QUEUE; h.n = 0;
    add(&h, KIND_ENQ, 1, 0, 0, 3);
    add(&h, KIND_ENQ, 2, 0, 1, 4);
    add(&h, KIND_DEQ, 0, 2, 5, 6);
    add(&h, KIND_DEQ, 0, 1, 7, 8);
    printf("queue, concurrent enqueues then dequeues 2,1: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(linearizable(&h, zero), "queue reorder of concurrent enq");

    h.n = 0;
    add(&h, KIND_ENQ, 1, 0, 0, 1);
    add(&h, KIND_ENQ, 2, 0, 2, 3);
    add(&h, KIND_DEQ, 0, 2, 4, 5);
    printf("queue, sequential enqueues 1,2 then dequeue 2: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(!linearizable(&h, zero), "queue FIFO violation");

    h.n = 0;
    add(&h, KIND_DEQ, 0, -1, 0, 5);
    add(&h, KIND_ENQ, 7, 0, 1, 2);
    printf("queue, dequeue overlapping an enqueue reports empty: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(linearizable(&h, zero), "empty deq overlapping enq");

    /* counter */
    h.model = MODEL_COUNTER; h.n = 0;
    add(&h, KIND_FAA, 1, 0, 0, 5);
    add(&h, KIND_FAA, 1, 0, 1, 6);
    printf("counter, two overlapping increments both returning 0: %s\n", linearizable(&h, zero) ? "linearizable" : "NOT linearizable");
    check(!linearizable(&h, zero), "duplicate ticket rejected");

    int ok_reg = 0, ok_cnt = 0, rej_reg = 0, rej_cnt = 0;
    for (int run = 0; run < 25; run++) {
        History r;
        build_recorded(&r, MODEL_REG);
        if (linearizable(&r, zero))
            ok_reg++;
        for (int i = 0; i < r.n; i++)
            if (r.op[i].kind == KIND_READ) {
                r.op[i].ret = 999; /* a value nobody wrote */
                break;
            }
        if (!linearizable(&r, zero))
            rej_reg++;
        build_recorded(&r, MODEL_COUNTER);
        if (linearizable(&r, zero))
            ok_cnt++;
        r.op[0].ret += 1000;
        if (!linearizable(&r, zero))
            rej_cnt++;
    }
    check(ok_reg == 25 && ok_cnt == 25, "recorded histories linearizable");
    check(rej_reg == 25 && rej_cnt == 25, "corrupted histories rejected");
    printf("recorded register histories linearizable: %d/25, corrupted rejected: %d/25\n", ok_reg, rej_reg);
    printf("recorded counter histories linearizable: %d/25, corrupted rejected: %d/25\n", ok_cnt, rej_cnt);
    return 0;
}
