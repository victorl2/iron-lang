/*
 * title: Promise groups with all, race and any combinators
 * topic: concurrency
 * covers: promise groups, first-resolved detection, first-success detection, error selection by index, resolution sequencer
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { NP = 8 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int resolved[NP], ok[NP];
    long value[NP];
    int nresolved, first, first_ok;
    /* sequencer: resolver with order position p may only resolve when turn == p */
    int turn;
} Group;

typedef struct {
    Group *g;
    int idx, pos, fail;
} Resolver;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void group_init(Group *g) {
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
    for (int i = 0; i < NP; i++)
        g->resolved[i] = g->ok[i] = 0, g->value[i] = 0;
    g->nresolved = 0;
    g->first = g->first_ok = -1;
    g->turn = 0;
}

static void group_destroy(Group *g) {
    pthread_mutex_destroy(&g->mu);
    pthread_cond_destroy(&g->cv);
}

static void *resolver(void *arg) {
    Resolver *r = arg;
    Group *g = r->g;
    pthread_mutex_lock(&g->mu);
    while (g->turn != r->pos)
        pthread_cond_wait(&g->cv, &g->mu);
    g->resolved[r->idx] = 1;
    g->ok[r->idx] = !r->fail;
    g->value[r->idx] = r->fail ? -1 : (long)r->idx * r->idx + 10;
    g->nresolved++;
    if (g->first < 0)
        g->first = r->idx;
    if (!r->fail && g->first_ok < 0)
        g->first_ok = r->idx;
    g->turn++;
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
    return NULL;
}

static int race(Group *g) {
    pthread_mutex_lock(&g->mu);
    while (g->first < 0)
        pthread_cond_wait(&g->cv, &g->mu);
    int f = g->first;
    pthread_mutex_unlock(&g->mu);
    return f;
}

/* first success, or -1 if every promise failed */
static int any(Group *g) {
    pthread_mutex_lock(&g->mu);
    while (g->first_ok < 0 && g->nresolved < NP)
        pthread_cond_wait(&g->cv, &g->mu);
    int f = g->first_ok;
    pthread_mutex_unlock(&g->mu);
    return f;
}

/* Waits for all; returns index of the lowest failed promise, or -1 with the value sum in *sum. */
static int all(Group *g, long *sum) {
    pthread_mutex_lock(&g->mu);
    while (g->nresolved < NP)
        pthread_cond_wait(&g->cv, &g->mu);
    int bad = -1;
    long s = 0;
    for (int i = 0; i < NP; i++) {
        if (!g->ok[i] && bad < 0)
            bad = i;
        s += g->value[i];
    }
    pthread_mutex_unlock(&g->mu);
    *sum = s;
    return bad;
}

static void scenario(const char *name, unsigned seed, unsigned fail_mask) {
    int perm[NP];
    for (int i = 0; i < NP; i++)
        perm[i] = i;
    unsigned s = seed;
    for (int i = NP - 1; i > 0; i--) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int j = (int)(s % (unsigned)(i + 1));
        int t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    Group g;
    group_init(&g);
    Resolver rs[NP];
    pthread_t th[NP];
    /* perm[pos] = index resolved at that position of the sequence */
    for (int pos = 0; pos < NP; pos++) {
        rs[pos].g = &g;
        rs[pos].idx = perm[pos];
        rs[pos].pos = pos;
        rs[pos].fail = (int)((fail_mask >> perm[pos]) & 1u);
        check(pthread_create(&th[pos], NULL, resolver, &rs[pos]) == 0, "create");
    }
    int r = race(&g);
    int a = any(&g);
    long sum;
    int bad = all(&g, &sum);
    for (int i = 0; i < NP; i++)
        pthread_join(th[i], NULL);
    int expect_any = -1;
    for (int pos = 0; pos < NP && expect_any < 0; pos++)
        if (!((fail_mask >> perm[pos]) & 1u))
            expect_any = perm[pos];
    int expect_bad = -1;
    for (int i = 0; i < NP && expect_bad < 0; i++)
        if ((fail_mask >> i) & 1u)
            expect_bad = i;
    check(r == perm[0], "race returns the first resolved");
    check(a == expect_any, "any returns the first success in resolution order");
    check(bad == expect_bad, "all reports the lowest failing index");
    printf("%s: resolution order", name);
    for (int i = 0; i < NP; i++)
        printf(" %d", perm[i]);
    printf("\n  race -> %d, any -> %d, all -> ", r, a);
    if (bad >= 0)
        printf("error at index %d\n", bad);
    else
        printf("values sum %ld\n", sum);
    group_destroy(&g);
}

int main(void) {
    scenario("no failures", 11u, 0u);
    scenario("some failures", 23u, 0x29u);
    scenario("first resolved fails", 7u, 0xffu & ~0x10u);
    scenario("all fail", 5u, 0xffu);
    scenario("last index fails", 99u, 0x80u);
    return 0;
}
