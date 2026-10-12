/*
 * title: Error propagation through a deep call chain without leaks
 * topic: memory
 * covers: error codes, trace frames, ownership passing, failure injection at every allocation, cleanup
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long ncalls, fail_at, live;
static void *xm(size_t n) {
    ncalls++;
    if (fail_at && ncalls == fail_at) return NULL;
    void *p = malloc(n);
    if (p) live++;
    return p;
}
static void xf(void *p) { if (p) { live--; free(p); } }

typedef enum { OK, ENOMEM_, EFORMAT, ERANGE_ } Err;
static const char *ename(Err e) {
    switch (e) { case OK: return "OK"; case ENOMEM_: return "NOMEM"; case EFORMAT: return "FORMAT"; default: return "RANGE"; }
}

/* trace of frames visited while unwinding */
static char trace[128];
static void note(const char *fn) {
    size_t l = strlen(trace);
    snprintf(trace + l, sizeof trace - l, "%s%s", l ? "<" : "", fn);
}

typedef struct { int *vals; int n; } Series;
typedef struct { Series *s; char *name; } Record;

static Err series_new(Series **out, int n) {
    Series *s = xm(sizeof *s);
    if (!s) { note("series_new"); return ENOMEM_; }
    s->vals = xm((size_t)n * sizeof(int));
    if (!s->vals) { xf(s); note("series_new"); return ENOMEM_; }
    s->n = n;
    *out = s;
    return OK;
}
static void series_free(Series *s) { if (s) { xf(s->vals); xf(s); } }

static Err record_new(Record **out, const char *name, int n) {
    if (n < 0) { note("record_new"); return ERANGE_; }
    Record *r = xm(sizeof *r);
    if (!r) { note("record_new"); return ENOMEM_; }
    Err e = series_new(&r->s, n);
    if (e) { xf(r); note("record_new"); return e; }
    r->name = xm(strlen(name) + 1);
    if (!r->name) { series_free(r->s); xf(r); note("record_new"); return ENOMEM_; }
    strcpy(r->name, name);
    for (int i = 0; i < n; i++) r->s->vals[i] = i * i;
    *out = r;
    return OK;
}
static void record_free(Record *r) { if (r) { xf(r->name); series_free(r->s); xf(r); } }

static Err parse_one(const char *spec, Record **out) {
    char name[16];
    int n;
    if (sscanf(spec, "%15[a-z]:%d", name, &n) != 2) { note("parse_one"); return EFORMAT; }
    Err e = record_new(out, name, n);
    if (e) note("parse_one");
    return e;
}

static Err load_all(const char *const *specs, int count, Record **recs) {
    int made = 0;
    for (int i = 0; i < count; i++) {
        Err e = parse_one(specs[i], &recs[i]);
        if (e) {
            while (made > 0) record_free(recs[--made]);
            note("load_all");
            return e;
        }
        made++;
    }
    return OK;
}

static Err summarize(const char *const *specs, int count, long *sum) {
    Record **recs = xm((size_t)count * sizeof *recs);
    if (!recs) { note("summarize"); return ENOMEM_; }
    Err e = load_all(specs, count, recs);
    if (e) { xf(recs); note("summarize"); return e; }
    long t = 0;
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < recs[i]->s->n; k++) t += recs[i]->s->vals[k];
        record_free(recs[i]);
    }
    xf(recs);
    *sum = t;
    return OK;
}

static Err run(const char *const *specs, int count, long *sum) {
    trace[0] = 0;
    Err e = summarize(specs, count, sum);
    if (e) note("run");
    return e;
}

int main(void) {
    const char *good[] = {"abc:4", "xy:3", "q:5"};
    const char *badfmt[] = {"abc:4", "!!:3"};
    const char *badrange[] = {"abc:4", "xy:-1"};
    long sum = 0;

    fail_at = 0; ncalls = 0;
    Err e = run(good, 3, &sum);
    long total = ncalls;
    printf("good input: %s sum=%ld allocs=%ld live=%ld\n", ename(e), sum, total, live);
    if (e || live) return 1;

    e = run(badfmt, 2, &sum);
    printf("bad format: %s trace=%s live=%ld\n", ename(e), trace, live);
    if (e != EFORMAT || live) return 1;
    e = run(badrange, 2, &sum);
    printf("bad range : %s trace=%s live=%ld\n", ename(e), trace, live);
    if (e != ERANGE_ || live) return 1;

    /* every allocation failure point over the good input */
    int shown = 0;
    for (long n = 1; n <= total; n++) {
        fail_at = n; ncalls = 0;
        e = run(good, 3, &sum);
        if (e != ENOMEM_ || live != 0) { fprintf(stderr, "fail point %ld: %s live=%ld\n", n, ename(e), live); return 1; }
        if (n == 1 || n == 2 || n == 3 || n == 4 || n == total) {
            printf("inject #%ld: %s trace=%s\n", n, ename(e), trace);
            shown++;
        }
    }
    fail_at = 0;
    printf("swept %ld failure points, %d shown, no leaks\n", total, shown);
    return 0;
}
