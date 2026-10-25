/*
 * title: Persistent array by Baker's rerooting trick
 * topic: data_structures
 * covers: persistent array, diff chains, rerooting, backtracking, cost of version switching
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 7770001u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

/* A version is either the real array or a diff pointing at a neighbour version. */
typedef struct V {
    int is_array;
    int *data;       /* when is_array */
    int idx, val;    /* when diff: this version = next with data[idx] = val */
    struct V *next;
} V;

#define LEN 24
static V **all;
static size_t all_n, all_cap;
static long reroot_steps;

static V *newv(void) {
    V *v = calloc(1, sizeof *v);
    CHECK(v);
    if (all_n == all_cap) {
        all_cap = all_cap ? all_cap * 2 : 256;
        all = realloc(all, all_cap * sizeof *all);
        CHECK(all);
    }
    all[all_n++] = v;
    return v;
}
static V *v_create(int init) {
    V *v = newv();
    v->is_array = 1;
    v->data = malloc(LEN * sizeof(int));
    CHECK(v->data);
    for (int i = 0; i < LEN; i++) v->data[i] = init;
    return v;
}
static void reroot(V *t) {
    if (t->is_array) return;
    V *n = t->next;
    reroot(n);
    reroot_steps++;
    /* n now owns the array; move ownership to t and make n a diff of t */
    int *d = n->data;
    int old = d[t->idx];
    d[t->idx] = t->val;
    t->is_array = 1;
    t->data = d;
    n->is_array = 0;
    n->data = NULL;
    n->idx = t->idx;
    n->val = old;
    n->next = t;
}
static int v_get(V *t, int i) {
    reroot(t);
    return t->data[i];
}
static V *v_set(V *t, int i, int val) {
    reroot(t);
    int *d = t->data;
    V *nv = newv();
    nv->is_array = 1;
    nv->data = d;
    int old = d[i];
    d[i] = val;
    t->is_array = 0;
    t->data = NULL;
    t->idx = i;
    t->val = old;
    t->next = nv;
    return nv;
}

#define SLOTS 10
int main(void) {
    V *ver[SLOTS];
    int model[SLOTS][LEN];
    ver[0] = v_create(0);
    for (int k = 0; k < LEN; k++) model[0][k] = 0;
    for (int i = 1; i < SLOTS; i++) {
        ver[i] = ver[0];
        for (int k = 0; k < LEN; k++) model[i][k] = 0;
    }
    long sets = 0, gets = 0;
    /* phase 1: linear (ephemeral-style) use, reroot cost stays zero */
    V *lin = ver[0];
    int lm[LEN] = {0};
    for (int s = 0; s < 500; s++) {
        int i = (int)(rnd() % LEN), val = (int)(rnd() % 100);
        lin = v_set(lin, i, val);
        lm[i] = val;
        CHECK(v_get(lin, (int)(rnd() % LEN)) >= 0);
        sets++; gets++;
    }
    for (int i = 0; i < LEN; i++) CHECK(v_get(lin, i) == lm[i]);
    printf("linear phase: %ld sets, reroot steps=%ld\n", sets, reroot_steps);
    CHECK(reroot_steps == 0);
    /* phase 2: branching versions, random access forces rerooting */
    for (int step = 0; step < 4000; step++) {
        int src = (int)(rnd() % SLOTS);
        int dst = (int)(rnd() % SLOTS);
        int i = (int)(rnd() % LEN);
        if (rnd() % 2) {
            int val = (int)(rnd() % 1000);
            V *n = v_set(ver[src], i, val);
            for (int k = 0; k < LEN; k++) model[dst][k] = model[src][k];
            model[dst][i] = val;
            ver[dst] = n;
            sets++;
        } else {
            CHECK(v_get(ver[src], i) == model[src][i]);
            gets++;
        }
    }
    for (int s = 0; s < SLOTS; s++)
        for (int i = 0; i < LEN; i++) CHECK(v_get(ver[s], i) == model[s][i]);
    /* phase 3: alternate between two distant versions: each switch pays the diff distance */
    long before = reroot_steps;
    for (int r = 0; r < 20; r++) {
        V *a = ver[r % 2];
        for (int i = 0; i < LEN; i += 5) CHECK(v_get(a, i) == model[r % 2][i]);
    }
    printf("versions created=%zu sets=%ld gets=%ld\n", all_n, sets, gets);
    printf("total reroot steps=%ld, ping-pong steps=%ld\n", reroot_steps, reroot_steps - before);
    int checksum = 0;
    for (int s = 0; s < SLOTS; s++)
        for (int i = 0; i < LEN; i++) checksum = (checksum * 31 + model[s][i]) % 1000003;
    printf("checksum of live versions=%d\n", checksum);
    for (size_t i = 0; i < all_n; i++) {
        if (all[i]->is_array) free(all[i]->data);
        free(all[i]);
    }
    free(all);
    return 0;
}
