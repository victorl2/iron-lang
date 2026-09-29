/*
 * title: Monotonic stack structure for spans and neighbors
 * topic: data_structures
 * covers: monotonic stack, stock span, next greater, previous smaller, online API, brute force check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x510E527FADE682D1ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 23); }

/* Stack of (index, value) with strictly decreasing values from bottom to top (keeps "previous greater"). */
typedef struct { int *idx; int *val; int n, cap; long pops; } MonoStack;
static void ms_init(MonoStack *s, int cap) { s->idx = malloc((size_t)cap * sizeof(int)); s->val = malloc((size_t)cap * sizeof(int)); CHECK(s->idx && s->val); s->n = 0; s->cap = cap; s->pops = 0; }
static void ms_free(MonoStack *s) { free(s->idx); free(s->val); }

/* Push value at position i. Returns the index of the previous element that is > v (or -1) and
 * pops everything <= v. The number of popped items plus one is the stock span. */
static int ms_push_decreasing(MonoStack *s, int i, int v) {
    while (s->n && s->val[s->n - 1] <= v) { s->n--; s->pops++; }
    int prev = s->n ? s->idx[s->n - 1] : -1;
    CHECK(s->n < s->cap);
    s->idx[s->n] = i; s->val[s->n] = v; s->n++;
    return prev;
}

/* Online stock spanner: span(i) = i - previous greater index */
typedef struct { MonoStack ms; int next; } Spanner;
static int span_next(Spanner *sp, int price) {
    int prev = ms_push_decreasing(&sp->ms, sp->next, price);
    return sp->next++ - prev;
}

int main(void) {
    enum { N = 3000 };
    static int a[N];
    for (int i = 0; i < N; i++) a[i] = (int)(rnd() % (i < 1000 ? 50 : 5000));

    /* online spans versus brute force */
    Spanner sp; ms_init(&sp.ms, N); sp.next = 0;
    long span_sum = 0; int span_max = 0, span_arg = 0;
    for (int i = 0; i < N; i++) {
        int s = span_next(&sp, a[i]);
        int bs = 1; while (i - bs >= 0 && a[i - bs] <= a[i]) bs++;
        CHECK(s == bs);
        span_sum += s;
        if (s > span_max) { span_max = s; span_arg = i; }
    }
    /* each element popped at most once: amortized O(n) */
    CHECK(sp.ms.pops <= N);
    printf("spans: sum=%ld max=%d at=%d pops=%ld final_stack=%d\n", span_sum, span_max, span_arg, sp.ms.pops, sp.ms.n);
    ms_free(&sp.ms);

    /* next greater element to the right by scanning right-to-left */
    static int ng[N], ps[N];
    MonoStack s; ms_init(&s, N);
    for (int i = N - 1; i >= 0; i--) {
        while (s.n && s.val[s.n - 1] <= a[i]) s.n--;
        ng[i] = s.n ? s.idx[s.n - 1] : -1;
        s.idx[s.n] = i; s.val[s.n] = a[i]; s.n++;
    }
    /* previous strictly smaller, increasing stack */
    s.n = 0;
    for (int i = 0; i < N; i++) {
        while (s.n && s.val[s.n - 1] >= a[i]) s.n--;
        ps[i] = s.n ? s.idx[s.n - 1] : -1;
        s.idx[s.n] = i; s.val[s.n] = a[i]; s.n++;
    }
    long ng_none = 0, ng_dist = 0, ps_none = 0, ps_dist = 0;
    for (int i = 0; i < N; i++) {
        int bg = -1; for (int j = i + 1; j < N; j++) if (a[j] > a[i]) { bg = j; break; }
        int bp = -1; for (int j = i - 1; j >= 0; j--) if (a[j] < a[i]) { bp = j; break; }
        CHECK(ng[i] == bg && ps[i] == bp);
        if (bg < 0) ng_none++; else ng_dist += bg - i;
        if (bp < 0) ps_none++; else ps_dist += i - bp;
    }
    printf("next greater: none=%ld total_distance=%ld\n", ng_none, ng_dist);
    printf("prev smaller: none=%ld total_distance=%ld\n", ps_none, ps_dist);
    ms_free(&s);

    /* daily temperatures style: days until warmer, on a small explicit example */
    static const int temps[] = { 73, 74, 75, 71, 69, 72, 76, 73 };
    int wait[8]; MonoStack w; ms_init(&w, 8);
    for (int i = 0; i < 8; i++) {
        wait[i] = 0;
        while (w.n && w.val[w.n - 1] < temps[i]) { w.n--; wait[w.idx[w.n]] = i - w.idx[w.n]; }
        w.idx[w.n] = i; w.val[w.n] = temps[i]; w.n++;
    }
    printf("days until warmer:");
    for (int i = 0; i < 8; i++) printf(" %d", wait[i]);
    printf("\n");
    ms_free(&w);
    return 0;
}
