/*
 * title: Reserve-then-commit batch insertion with the strong guarantee
 * topic: memory
 * covers: two-phase insert, up-front reservation, no partial effects on failure, failure sweep, ownership hand-off of strings
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long ncalls, fail_at, live;
static void *xm(size_t n) { ncalls++; if (fail_at && ncalls == fail_at) return NULL; void *p = malloc(n); if (p) live++; return p; }
static void xf(void *p) { if (p) { live--; free(p); } }

typedef struct { char **items; size_t n, cap; } List;

static void list_free(List *l) { for (size_t i = 0; i < l->n; i++) xf(l->items[i]); xf(l->items); l->items = NULL; l->n = l->cap = 0; }

/* reserve capacity in a new array; on failure the list is unchanged */
static int list_reserve(List *l, size_t extra) {
    if (l->n + extra <= l->cap) return 1;
    size_t nc = l->cap ? l->cap : 4;
    while (nc < l->n + extra) nc *= 2;
    char **ni = xm(nc * sizeof *ni);
    if (!ni) return 0;
    if (l->n) memcpy(ni, l->items, l->n * sizeof *ni);
    xf(l->items);
    l->items = ni; l->cap = nc;
    return 1;
}

/* Naive: inserts one by one; a failure midway leaves a prefix inserted. */
static int add_naive(List *l, const char *const *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!list_reserve(l, 1)) return 0;
        char *d = xm(strlen(v[i]) + 1);
        if (!d) return 0;
        strcpy(d, v[i]);
        l->items[l->n++] = d;
    }
    return 1;
}

/* Strong guarantee: (1) reserve slots, (2) make every copy into a temp array, (3) commit with no allocation. */
static int add_atomic(List *l, const char *const *v, size_t n) {
    if (!list_reserve(l, n)) return 0;
    char **tmp = xm(n * sizeof *tmp);
    if (!tmp) return 0;
    for (size_t i = 0; i < n; i++) {
        tmp[i] = xm(strlen(v[i]) + 1);
        if (!tmp[i]) {
            while (i) xf(tmp[--i]);
            xf(tmp);
            return 0;
        }
        strcpy(tmp[i], v[i]);
    }
    memcpy(l->items + l->n, tmp, n * sizeof *tmp); /* commit: cannot fail */
    l->n += n;
    xf(tmp);
    return 1;
}

static unsigned digest(const List *l) {
    unsigned h = 5381;
    for (size_t i = 0; i < l->n; i++) for (const char *c = l->items[i]; *c; c++) h = h * 33 + (unsigned char)*c;
    return h * 31 + (unsigned)l->n;
}

int main(void) {
    const char *seed[] = {"one", "two", "three"};
    const char *batch[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta"};
    const size_t nb = sizeof batch / sizeof batch[0];

    /* baseline allocation count of the atomic insert on a seeded list */
    List l = {0};
    add_atomic(&l, seed, 3);
    unsigned d0 = digest(&l);
    ncalls = 0; fail_at = 0;
    add_atomic(&l, batch, nb);
    long need = ncalls;
    printf("atomic add of %zu items: %ld allocations, list now %zu items\n", nb, need, l.n);
    list_free(&l);

    int atomic_ok = 0, naive_partial = 0, naive_total = 0;
    long naive_need;
    {
        List t = {0}; add_naive(&t, seed, 3);
        ncalls = 0; add_naive(&t, batch, nb); naive_need = ncalls;
        list_free(&t);
    }
    for (long n = 1; n <= need; n++) {
        List a = {0};
        fail_at = 0; add_atomic(&a, seed, 3);
        size_t before = a.n;
        fail_at = n; ncalls = 0;
        int rc = add_atomic(&a, batch, nb);
        /* reserve may have grown the array (invisible to callers), but contents and count are unchanged */
        if (rc == 0 && (a.n != before || digest(&a) != d0)) { fprintf(stderr, "atomic changed state at %ld\n", n); return 1; }
        if (rc == 0) atomic_ok++;
        fail_at = 0;
        list_free(&a);
    }
    for (long n = 1; n <= naive_need; n++) {
        List a = {0};
        fail_at = 0; add_naive(&a, seed, 3);
        fail_at = n; ncalls = 0;
        int rc = add_naive(&a, batch, nb);
        naive_total++;
        if (rc == 0 && a.n != 3) naive_partial++;
        fail_at = 0;
        list_free(&a);
    }
    printf("atomic: %d/%ld failure points left the list untouched\n", atomic_ok, need);
    printf("naive : %d/%d failure points left a partial batch behind\n", naive_partial, naive_total);
    printf("live after all runs: %ld\n", live);
    return (live == 0 && atomic_ok == need) ? 0 : 1;
}
