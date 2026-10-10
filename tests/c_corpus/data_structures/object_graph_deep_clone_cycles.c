/*
 * title: Cyclic object graphs: safe traversal, deep clone with forwarding pointers, isomorphism and SCCs
 * topic: data_structures
 * covers: cycle-safe DFS and BFS, visited marks by epoch, deep copy preserving sharing and cycles, canonical serialization, Tarjan SCC, transitive-closure oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 8888u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Obj {
    int id, payload;
    int nref;
    struct Obj **refs;
    struct Obj *fwd;       /* forwarding pointer used while cloning */
    int epoch;             /* visited stamp */
    int color, index, low, on_stack; /* scratch for cycle detection and Tarjan */
} Obj;

#define MAXN 64
static int cur_epoch;

static Obj *obj_new(int id, int payload, int nref) {
    Obj *o = calloc(1, sizeof *o);
    CHECK(o);
    o->id = id; o->payload = payload; o->nref = nref;
    o->refs = calloc((size_t)(nref ? nref : 1), sizeof(Obj *));
    CHECK(o->refs);
    return o;
}
static void obj_free(Obj *o) { free(o->refs); free(o); }

/* iterative DFS with an explicit stack: preorder list of reachable objects */
static int reach(Obj *root, Obj **order) {
    Obj *stack[MAXN * 8];
    int sp = 0, n = 0;
    cur_epoch++;
    stack[sp++] = root;
    root->epoch = cur_epoch;
    while (sp) {
        Obj *o = stack[--sp];
        order[n++] = o;
        for (int i = o->nref - 1; i >= 0; i--) {
            Obj *c = o->refs[i];
            if (c && c->epoch != cur_epoch) { c->epoch = cur_epoch; stack[sp++] = c; CHECK(sp < MAXN * 8); }
        }
    }
    return n;
}
/* three-color DFS: a back edge to a gray node is a cycle */
static int has_cycle(Obj *o) {
    o->color = 1;
    for (int i = 0; i < o->nref; i++) {
        Obj *c = o->refs[i];
        if (!c) continue;
        if (c->color == 1) return 1;
        if (c->color == 0 && has_cycle(c)) return 1;
    }
    o->color = 2;
    return 0;
}
static int tarjan_counter, scc_count;
static Obj *tstack[MAXN];
static int tsp;
static void tarjan(Obj *v) {
    v->index = v->low = ++tarjan_counter;
    tstack[tsp++] = v; v->on_stack = 1;
    for (int i = 0; i < v->nref; i++) {
        Obj *w = v->refs[i];
        if (!w) continue;
        if (!w->index) { tarjan(w); if (w->low < v->low) v->low = w->low; }
        else if (w->on_stack && w->index < v->low) v->low = w->index;
    }
    if (v->low == v->index) {
        Obj *w;
        do { w = tstack[--tsp]; w->on_stack = 0; } while (w != v);
        scc_count++;
    }
}
/* deep clone: the forwarding pointer both breaks cycles and preserves sharing */
static Obj *clone(Obj *o, Obj **made, int *nmade) {
    if (o->fwd) return o->fwd;
    Obj *c = obj_new(o->id, o->payload, o->nref);
    o->fwd = c;
    made[(*nmade)++] = c;
    for (int i = 0; i < o->nref; i++) c->refs[i] = o->refs[i] ? clone(o->refs[i], made, nmade) : NULL;
    return c;
}
/* canonical text: number objects in BFS discovery order */
static int serialize(Obj *root, char *out, size_t cap) {
    Obj *queue[MAXN];
    int head = 0, tail = 0;
    cur_epoch++;
    root->epoch = cur_epoch; root->index = 0;
    queue[tail++] = root;
    size_t n = 0;
    while (head < tail) {
        Obj *o = queue[head++];
        n += (size_t)snprintf(out + n, cap - n, "%d[", o->payload);
        for (int i = 0; i < o->nref; i++) {
            Obj *c = o->refs[i];
            if (!c) { n += (size_t)snprintf(out + n, cap - n, "-"); }
            else {
                if (c->epoch != cur_epoch) { c->epoch = cur_epoch; c->index = tail; queue[tail++] = c; }
                n += (size_t)snprintf(out + n, cap - n, "%d", c->index);
            }
            if (i + 1 < o->nref) n += (size_t)snprintf(out + n, cap - n, ",");
        }
        n += (size_t)snprintf(out + n, cap - n, "]");
        CHECK(n + 16 < cap);
    }
    return tail;
}

int main(void) {
    long total_reach = 0, cyclic = 0, total_scc = 0, total_shared = 0;
    for (int trial = 0; trial < 60; trial++) {
        int n = 5 + (int)(rnd() % (MAXN - 5));
        int maxref = 2 + (int)(rnd() % 2);
        int density = 60 + (int)(rnd() % 40);
        Obj *objs[MAXN];
        static int adj[MAXN][MAXN];
        memset(adj, 0, sizeof adj);
        for (int i = 0; i < n; i++) {
            int val = (int)(rnd() % 1000);
            int nref = 1 + (int)(rnd() % (unsigned)maxref);
            objs[i] = obj_new(i, val, nref);
        }
        for (int i = 0; i < n; i++)
            for (int k = 0; k < objs[i]->nref; k++)
                if ((int)(rnd() % 100) < density) {
                    int t = (int)(rnd() % (unsigned)n);
                    objs[i]->refs[k] = objs[t];
                    adj[i][t]++;
                }
        for (int k = 0; k < objs[0]->nref; k++)
            if (!objs[0]->refs[k]) {
                int t = 1 + (int)(rnd() % (unsigned)(n - 1));
                objs[0]->refs[k] = objs[t];
                adj[0][t]++;
            }
        Obj *root = objs[0];
        /* oracle: transitive closure by Warshall over "path of length >= 1" */
        static unsigned char cl[MAXN][MAXN];
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) cl[i][j] = adj[i][j] > 0;
        for (int k = 0; k < n; k++) for (int i = 0; i < n; i++) if (cl[i][k]) for (int j = 0; j < n; j++) if (cl[k][j]) cl[i][j] = 1;
        unsigned char inreach[MAXN];
        for (int j = 0; j < n; j++) inreach[j] = (unsigned char)(j == 0 || cl[0][j]);
        int expect_reach = 0, expect_cycle = 0;
        for (int j = 0; j < n; j++) { expect_reach += inreach[j]; if (inreach[j] && cl[j][j]) expect_cycle = 1; }
        int expect_scc = 0;
        for (int j = 0; j < n; j++) {
            if (!inreach[j]) continue;
            int rep = 1;
            for (int k = 0; k < j; k++) if (inreach[k] && cl[j][k] && cl[k][j]) rep = 0;
            if (rep) expect_scc++;
        }

        Obj *order[MAXN];
        int r = reach(root, order);
        CHECK(r == expect_reach);
        for (int i = 0; i < r; i++) CHECK(inreach[order[i]->id]);
        for (int i = 0; i < n; i++) objs[i]->color = 0;
        CHECK(has_cycle(root) == expect_cycle);
        for (int i = 0; i < n; i++) { objs[i]->index = objs[i]->low = objs[i]->on_stack = 0; }
        tarjan_counter = 0; scc_count = 0; tsp = 0;
        tarjan(root);
        CHECK(scc_count == expect_scc);

        Obj *made[MAXN];
        int nmade = 0;
        Obj *c = clone(root, made, &nmade);
        CHECK(nmade == r);
        for (int i = 0; i < n; i++) objs[i]->fwd = NULL;
        /* the clone shares nothing with the original and has identical shape */
        for (int i = 0; i < nmade; i++) for (int j = 0; j < n; j++) CHECK(made[i] != objs[j]);
        char s1[8192], s2[8192];
        int n1 = serialize(root, s1, sizeof s1), n2 = serialize(c, s2, sizeof s2);
        CHECK(n1 == n2 && strcmp(s1, s2) == 0 && n1 == r);
        /* mutate the clone, original must not change, then the texts must differ (unless nothing changed) */
        int old_payload = root->payload;
        c->payload += 1;
        CHECK(root->payload == old_payload);
        serialize(c, s2, sizeof s2);
        CHECK(strcmp(s1, s2) != 0);
        /* sharing: objects with more than one incoming edge inside the reachable part stay shared in the clone */
        int shared = 0;
        for (int j = 0; j < n; j++) {
            if (!inreach[j]) continue;
            int in = 0;
            for (int i = 0; i < n; i++) if (inreach[i]) in += adj[i][j];
            if (in > 1) shared++;
        }
        Obj *co[MAXN];
        int cn = reach(c, co);
        CHECK(cn == r);
        int cshared = 0;
        for (int i = 0; i < cn; i++) {
            int in = 0;
            for (int k = 0; k < cn; k++) for (int q = 0; q < co[k]->nref; q++) if (co[k]->refs[q] == co[i]) in++;
            if (in > 1) cshared++;
        }
        CHECK(cshared == shared);
        total_reach += r; cyclic += expect_cycle; total_scc += scc_count; total_shared += shared;
        if (trial % 12 == 0)
            printf("trial %2d: %2d objects, %2d reachable, %2d SCCs, %2d shared, cycle=%d, text length %zu\n", trial, n, r, scc_count, shared, expect_cycle, strlen(s1));
        for (int i = 0; i < nmade; i++) obj_free(made[i]);
        for (int i = 0; i < n; i++) obj_free(objs[i]);
    }
    printf("60 graphs: reachable=%ld cyclic=%ld SCCs=%ld shared=%ld\n", total_reach, cyclic, total_scc, total_shared);
    return 0;
}
