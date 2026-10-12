/*
 * title: goto cleanup with fault injection at every step
 * topic: memory
 * covers: goto error unwinding, multiple resources, injected allocation failures, leak accounting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_at = -1; /* fail the Nth acquisition (0-based) */
static int acq_count;
static int live_resources;
static int total_acquired;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Every acquisition goes through here so tests can force a failure. */
static void *acquire(size_t n) {
    if (acq_count++ == fail_at)
        return NULL;
    void *p = malloc(n);
    check(p != NULL, "real alloc");
    live_resources++;
    total_acquired++;
    return p;
}

static void relinquish(void *p) {
    if (p) {
        free(p);
        live_resources--;
    }
}

typedef struct {
    int *table;
    char *name;
    double *weights;
    unsigned char *scratch;
    int **rows;
    int nrows;
} Engine;

/* Returns 0 on success, or the (1-based) step that failed. */
static int engine_build(Engine *e, int nrows) {
    int step = 0;
    memset(e, 0, sizeof *e);

    step = 1;
    e->table = acquire(64 * sizeof(int));
    if (!e->table)
        goto fail;
    step = 2;
    e->name = acquire(32);
    if (!e->name)
        goto fail;
    step = 3;
    e->weights = acquire(16 * sizeof(double));
    if (!e->weights)
        goto fail;
    step = 4;
    e->scratch = acquire(128);
    if (!e->scratch)
        goto fail;
    step = 5;
    e->rows = acquire((size_t)nrows * sizeof(int *));
    if (!e->rows)
        goto fail;
    for (int i = 0; i < nrows; i++) {
        step = 6;
        e->rows[i] = acquire(8 * sizeof(int));
        if (!e->rows[i])
            goto fail_rows;
        e->nrows++;
    }
    for (int i = 0; i < 64; i++)
        e->table[i] = i;
    snprintf(e->name, 32, "engine-%d", nrows);
    return 0;

fail_rows:
    while (e->nrows > 0)
        relinquish(e->rows[--e->nrows]);
fail:
    relinquish(e->rows);
    relinquish(e->scratch);
    relinquish(e->weights);
    relinquish(e->name);
    relinquish(e->table);
    memset(e, 0, sizeof *e);
    return step;
}

static void engine_destroy(Engine *e) {
    while (e->nrows > 0)
        relinquish(e->rows[--e->nrows]);
    relinquish(e->rows);
    relinquish(e->scratch);
    relinquish(e->weights);
    relinquish(e->name);
    relinquish(e->table);
    memset(e, 0, sizeof *e);
}

int main(void) {
    const int nrows = 4;
    /* total acquisitions on success: 5 + nrows */
    int steps = 5 + nrows;
    printf("fault injection over %d acquisitions\n", steps);
    for (fail_at = 0; fail_at <= steps; fail_at++) {
        Engine e;
        acq_count = 0;
        total_acquired = 0;
        int r = engine_build(&e, nrows);
        printf("fail_at=%d -> %s (step %d), acquired=%d, live=%d\n", fail_at,
               r ? "failed" : "ok", r, total_acquired, live_resources);
        if (fail_at < steps) {
            check(r != 0, "expected failure");
            check(live_resources == 0, "leak after failure");
        } else {
            check(r == 0, "expected success");
            check(live_resources == steps, "all live");
            check(strcmp(e.name, "engine-4") == 0 && e.table[63] == 63, "contents");
            engine_destroy(&e);
            check(live_resources == 0, "leak after destroy");
        }
    }
    return 0;
}
