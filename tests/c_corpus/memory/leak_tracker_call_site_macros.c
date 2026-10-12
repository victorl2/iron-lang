/*
 * title: Leak tracker keyed by call site via __LINE__ and __func__ macros
 * topic: memory
 * covers: macro wrappers, __LINE__, __func__, per-site aggregation, leak report, double free detection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Rec {
    void *p;
    size_t n;
    const char *fn;
    int line;
    struct Rec *next;
} Rec;

static Rec *live_list;
static long bad_frees;

static void *track_alloc(size_t n, const char *fn, int line) {
    Rec *r = malloc(sizeof *r);
    void *p = malloc(n ? n : 1);
    if (!r || !p) { free(r); free(p); return NULL; }
    r->p = p; r->n = n; r->fn = fn; r->line = line; r->next = live_list;
    live_list = r;
    return p;
}

static void track_free(void *p, const char *fn, int line) {
    if (!p) return;
    for (Rec **pp = &live_list; *pp; pp = &(*pp)->next) {
        if ((*pp)->p == p) {
            Rec *r = *pp;
            *pp = r->next;
            free(r->p);
            free(r);
            return;
        }
    }
    bad_frees++;
    printf("bad free from %s:%d\n", fn, line);
}

#define MALLOC(n) track_alloc((n), __func__, __LINE__)
#define FREE(p) track_free((p), __func__, __LINE__)

/* a fixed set of sites that each allocate a known amount */
static void *site_a(void) { return MALLOC(10); }
static void *site_b(int n) { return MALLOC((size_t)n * 4); }
static char *dup_str(const char *s) {
    char *d = MALLOC(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

typedef struct { const char *fn; int line; long count; size_t bytes; } Site;

static int site_cmp_key(const Site *a, const Site *b) {
    int c = strcmp(a->fn, b->fn);
    if (c) return c;
    return a->line - b->line;
}

static void report(const char *title) {
    Site sites[32];
    int ns = 0;
    for (Rec *r = live_list; r; r = r->next) {
        int i;
        for (i = 0; i < ns; i++)
            if (strcmp(sites[i].fn, r->fn) == 0 && sites[i].line == r->line) break;
        if (i == ns) { sites[ns].fn = r->fn; sites[ns].line = r->line; sites[ns].count = 0; sites[ns].bytes = 0; ns++; }
        sites[i].count++;
        sites[i].bytes += r->n;
    }
    /* insertion sort by (function, line): a total order since sites are unique */
    for (int i = 1; i < ns; i++) {
        Site x = sites[i];
        int j = i - 1;
        while (j >= 0 && site_cmp_key(&sites[j], &x) > 0) { sites[j + 1] = sites[j]; j--; }
        sites[j + 1] = x;
    }
    printf("%s: %d site(s) with live blocks\n", title, ns);
    for (int i = 0; i < ns; i++)
        printf("  %-10s line %d count=%ld bytes=%zu\n", sites[i].fn, sites[i].line, sites[i].count, sites[i].bytes);
}

static void leaky_worker(int n) {
    for (int i = 0; i < n; i++) {
        void *a = site_a();
        char *s = dup_str("hello");
        FREE(a);
        if (i % 2 == 0) FREE(s); /* odd iterations leak the string */
    }
}

int main(void) {
    void *keep1 = site_a();
    void *keep2 = site_b(25);
    leaky_worker(7);
    report("before cleanup");

    FREE(keep1);
    FREE(keep2);
    FREE(keep2); /* double free: the tracker refuses it */
    int stray;
    FREE(&stray); /* foreign pointer */
    printf("bad frees detected: %ld\n", bad_frees);
    report("after partial cleanup");

    long freed = 0;
    while (live_list) {
        Rec *r = live_list;
        void *p = r->p;
        FREE(p);
        freed++;
    }
    printf("swept %ld leaked blocks\n", freed);
    report("final");
    return bad_frees == 2 && freed == 3 ? 0 : 1;
}
