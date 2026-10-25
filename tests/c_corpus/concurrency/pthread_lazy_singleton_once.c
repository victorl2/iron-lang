/*
 * title: Thread-safe lazy singletons
 * topic: concurrency
 * covers: double-checked locking with C11 atomics, pthread_once singleton, construction counts
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 8 };

typedef struct {
    int serial;
    char name[24];
    long table[16];
} Config;

static atomic_int constructed_a, constructed_b;

/* Variant A: double-checked locking with acquire/release atomics. */
static _Atomic(Config *) inst_a;
static pthread_mutex_t a_mu = PTHREAD_MUTEX_INITIALIZER;

static Config *build(atomic_int *counter, const char *name) {
    Config *c = calloc(1, sizeof *c);
    c->serial = atomic_fetch_add(counter, 1) + 1;
    snprintf(c->name, sizeof c->name, "%s", name);
    for (int i = 0; i < 16; i++)
        c->table[i] = (long)i * i + 7;
    return c;
}

static Config *get_a(void) {
    Config *c = atomic_load_explicit(&inst_a, memory_order_acquire);
    if (!c) {
        pthread_mutex_lock(&a_mu);
        c = atomic_load_explicit(&inst_a, memory_order_relaxed);
        if (!c) {
            c = build(&constructed_a, "double-checked");
            atomic_store_explicit(&inst_a, c, memory_order_release);
        }
        pthread_mutex_unlock(&a_mu);
    }
    return c;
}

/* Variant B: pthread_once. */
static pthread_once_t b_once = PTHREAD_ONCE_INIT;
static Config *inst_b;

static void make_b(void) {
    inst_b = build(&constructed_b, "once");
}

static Config *get_b(void) {
    pthread_once(&b_once, make_b);
    return inst_b;
}

typedef struct {
    Config *a, *b;
    long sum;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *worker(void *p) {
    Arg *r = p;
    r->a = get_a();
    r->b = get_b();
    for (int i = 0; i < 16; i++)
        r->sum += r->a->table[i] + r->b->table[i];
    return NULL;
}

int main(void) {
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        memset(&args[i], 0, sizeof args[i]);
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    long expect = 0;
    for (int i = 0; i < 16; i++)
        expect += 2 * ((long)i * i + 7);
    for (int i = 0; i < T; i++) {
        check(args[i].a == args[0].a, "same instance A");
        check(args[i].b == args[0].b, "same instance B");
        check(args[i].sum == expect, "sum");
    }
    check(atomic_load(&constructed_a) == 1, "A built once");
    check(atomic_load(&constructed_b) == 1, "B built once");
    printf("A: %s serial=%d built=%d\n", args[0].a->name, args[0].a->serial, atomic_load(&constructed_a));
    printf("B: %s serial=%d built=%d\n", args[0].b->name, args[0].b->serial, atomic_load(&constructed_b));
    printf("per-thread sum=%ld\n", expect);
    free(args[0].a);
    free(args[0].b);
    return 0;
}
