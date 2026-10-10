/*
 * title: Lazy memoized streams: primes, Fibonacci and Hamming numbers
 * topic: data_structures
 * covers: thunks, memoization, infinite streams, self-referential definitions, lazy map filter zip merge
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

typedef struct Stream Stream;
typedef struct Thunk Thunk;
struct Thunk { int forced; Stream *val; Stream *(*fn)(void *env); void *env; };
struct Stream { long head; Thunk *tail; };

static void **arena;
static size_t arena_n, arena_cap;
static long thunk_evals;

static void *xa(size_t n) {
    void *p = calloc(1, n);
    CHECK(p);
    if (arena_n == arena_cap) {
        arena_cap = arena_cap ? arena_cap * 2 : 1024;
        arena = realloc(arena, arena_cap * sizeof *arena);
        CHECK(arena);
    }
    arena[arena_n++] = p;
    return p;
}
static Thunk *thunk(Stream *(*fn)(void *), void *env) {
    Thunk *t = xa(sizeof *t);
    t->fn = fn; t->env = env;
    return t;
}
static Stream *cons(long h, Thunk *t) {
    Stream *s = xa(sizeof *s);
    s->head = h; s->tail = t;
    return s;
}
static Stream *force(Thunk *t) {
    if (!t->forced) {
        thunk_evals++;
        t->val = t->fn(t->env);
        t->forced = 1;
    }
    return t->val;
}
static Stream *tl(Stream *s) { return force(s->tail); }

/* from(n) = n : from(n + 1) */
static Stream *from(long n);
static Stream *from_step(void *env) { return from(*(long *)env); }
static Stream *from(long n) {
    long *e = xa(sizeof *e);
    *e = n + 1;
    return cons(n, thunk(from_step, e));
}

/* map */
typedef struct { long (*f)(long); Stream *s; } MapEnv;
static Stream *smap(long (*f)(long), Stream *s);
static Stream *map_step(void *env) { MapEnv *e = env; return smap(e->f, tl(e->s)); }
static Stream *smap(long (*f)(long), Stream *s) {
    MapEnv *e = xa(sizeof *e);
    e->f = f; e->s = s;
    return cons(f(s->head), thunk(map_step, e));
}

/* filter: skip forward until a match, then lazily continue */
typedef struct { int (*p)(long, void *); void *ctx; Stream *s; } FiltEnv;
static Stream *sfilter(int (*p)(long, void *), void *ctx, Stream *s);
static Stream *filt_step(void *env) { FiltEnv *e = env; return sfilter(e->p, e->ctx, tl(e->s)); }
static Stream *sfilter(int (*p)(long, void *), void *ctx, Stream *s) {
    while (!p(s->head, ctx)) s = tl(s);
    FiltEnv *e = xa(sizeof *e);
    e->p = p; e->ctx = ctx; e->s = s;
    return cons(s->head, thunk(filt_step, e));
}

/* zip with */
typedef struct { long (*f)(long, long); Stream *a, *b; } ZipEnv;
static Stream *szip(long (*f)(long, long), Stream *a, Stream *b);
static Stream *zip_step(void *env) { ZipEnv *e = env; return szip(e->f, tl(e->a), tl(e->b)); }
static Stream *szip(long (*f)(long, long), Stream *a, Stream *b) {
    ZipEnv *e = xa(sizeof *e);
    e->f = f; e->a = a; e->b = b;
    return cons(f(a->head, b->head), thunk(zip_step, e));
}

/* ordered merge without duplicates */
typedef struct { Stream *a, *b; } MergeEnv;
static Stream *smerge(Stream *a, Stream *b);
static Stream *merge_step(void *env) {
    MergeEnv *e = env;
    Stream *a = e->a, *b = e->b;
    if (a->head < b->head) return smerge(tl(a), b);
    if (a->head > b->head) return smerge(a, tl(b));
    return smerge(tl(a), tl(b));
}
static Stream *smerge(Stream *a, Stream *b) {
    MergeEnv *e = xa(sizeof *e);
    e->a = a; e->b = b;
    long h = a->head < b->head ? a->head : b->head;
    return cons(h, thunk(merge_step, e));
}

/* sieve: p : sieve(filter (not multiple of p) rest) */
static int not_multiple(long x, void *ctx) { return x % *(long *)ctx != 0; }
static Stream *sieve(Stream *s);
static Stream *sieve_step(void *env) {
    Stream *s = env;
    long *p = xa(sizeof *p);
    *p = s->head;
    return sieve(sfilter(not_multiple, p, tl(s)));
}
static Stream *sieve(Stream *s) { return cons(s->head, thunk(sieve_step, s)); }

/* Fibonacci: 0 : 1 : zipWith (+) fibs (tail fibs), knotted through pointers */
static long add(long a, long b) { return a + b; }
typedef struct { Stream *f0; Stream *f1; } FibEnv;
static Stream *fib_tail2(void *env) { FibEnv *e = env; return szip(add, e->f0, e->f1); }
static Stream *fib_tail1(void *env) {
    Stream *f0 = env;
    FibEnv *e = xa(sizeof *e);
    Stream *f1 = cons(1, NULL);
    e->f0 = f0; e->f1 = f1;
    f1->tail = thunk(fib_tail2, e);
    return f1;
}
static Stream *fibs(void) {
    Stream *f0 = cons(0, NULL);
    f0->tail = thunk(fib_tail1, f0);
    return f0;
}

/* Hamming numbers: 1 : merge(2*h, merge(3*h, 5*h)) */
static long times2(long x) { return x * 2; }
static long times3(long x) { return x * 3; }
static long times5(long x) { return x * 5; }
static Stream *ham_tail(void *env) {
    Stream *h = env;
    return smerge(smap(times2, h), smerge(smap(times3, h), smap(times5, h)));
}
static Stream *hamming(void) {
    Stream *h = cons(1, NULL);
    h->tail = thunk(ham_tail, h);
    return h;
}
static long square(long x) { return x * x; }
static int is_even(long x, void *ctx) { (void)ctx; return x % 2 == 0; }

static Stream *nth_cell(Stream *s, int n) { while (n-- > 0) s = tl(s); return s; }

int main(void) {
    /* map/filter over the naturals */
    Stream *nat = from(0);
    Stream *sq_even = smap(square, sfilter(is_even, NULL, nat));
    printf("even squares:");
    Stream *s = sq_even;
    for (int i = 0; i < 8; i++, s = tl(s)) { printf(" %ld", s->head); CHECK(s->head == (long)(2 * i) * (2 * i)); }
    printf("\n");

    /* sieve against trial division */
    thunk_evals = 0;
    Stream *pr = sieve(from(2));
    printf("primes:");
    s = pr;
    int count = 0;
    long cand = 2;
    for (int i = 0; i < 40; i++, s = tl(s)) {
        int isp;
        do {
            isp = 1;
            for (long d = 2; d * d <= cand; d++) if (cand % d == 0) { isp = 0; break; }
            if (!isp) cand++;
        } while (!isp);
        CHECK(s->head == cand);
        cand++;
        count++;
        if (i < 15) printf(" %ld", s->head);
    }
    printf(" ... p%d=%ld\n", count, nth_cell(pr, 39)->head);

    /* Fibonacci: memoization means the number of thunk evaluations grows linearly */
    long before = thunk_evals;
    Stream *f = fibs();
    long a = 0, b = 1;
    s = f;
    for (int i = 0; i < 70; i++, s = tl(s)) {
        CHECK(s->head == a);
        long t = a + b; a = b; b = t;
    }
    long fib_evals = thunk_evals - before;
    printf("fib(69)=%ld with %ld thunk evaluations\n", nth_cell(f, 69)->head, fib_evals);
    CHECK(fib_evals <= 3 * 70);
    long again = thunk_evals;
    (void)nth_cell(f, 69);
    CHECK(thunk_evals == again); /* re-walking a memoized stream forces nothing */

    /* Hamming numbers against enumeration of 2^i 3^j 5^k */
    Stream *h = hamming();
    long brute[2000];
    int nb = 0;
    for (long p2 = 1; p2 <= 10000000L; p2 *= 2)
        for (long p3 = p2; p3 <= 10000000L; p3 *= 3)
            for (long p5 = p3; p5 <= 10000000L; p5 *= 5) brute[nb++] = p5;
    for (int i = 1; i < nb; i++) { long v = brute[i]; int j = i - 1; while (j >= 0 && brute[j] > v) { brute[j + 1] = brute[j]; j--; } brute[j + 1] = v; }
    printf("hamming:");
    s = h;
    for (int i = 0; i < 300; i++, s = tl(s)) {
        CHECK(s->head == brute[i]);
        if (i < 20) printf(" %ld", s->head);
    }
    printf(" ... h300=%ld\n", nth_cell(h, 299)->head);
    printf("total thunk evaluations: %ld, allocations: %zu\n", thunk_evals, arena_n);
    for (size_t i = 0; i < arena_n; i++) free(arena[i]);
    free(arena);
    return 0;
}
