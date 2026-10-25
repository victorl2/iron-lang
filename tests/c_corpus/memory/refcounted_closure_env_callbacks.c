/*
 * title: Closures with refcounted captured environments
 * topic: memory
 * covers: closure emulation (function pointer + env), shared captured state, env lifetime via refcount, callbacks stored and invoked later
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int rc;
    int counter;
    long total;
    char name[12];
} Env;

typedef struct Closure Closure;
struct Closure {
    long (*fn)(Env *, long);
    Env *env; /* retained */
};

static int live_envs, live_closures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Env *env_new(const char *name) {
    Env *e = calloc(1, sizeof *e);
    check(e != NULL, "alloc");
    e->rc = 1;
    snprintf(e->name, sizeof e->name, "%s", name);
    live_envs++;
    return e;
}

static Env *env_retain(Env *e) {
    e->rc++;
    return e;
}

static void env_release(Env *e) {
    if (--e->rc == 0) {
        printf("  env '%s' freed (counter=%d total=%ld)\n", e->name, e->counter, e->total);
        free(e);
        live_envs--;
    }
}

static Closure *closure_new(long (*fn)(Env *, long), Env *env) {
    Closure *c = malloc(sizeof *c);
    check(c != NULL, "alloc");
    c->fn = fn;
    c->env = env_retain(env);
    live_closures++;
    return c;
}

static void closure_free(Closure *c) {
    env_release(c->env);
    free(c);
    live_closures--;
}

static long call(Closure *c, long arg) { return c->fn(c->env, arg); }

/* Two closures share one env: an accumulator and a reader. */
static long fn_add(Env *e, long x) {
    e->counter++;
    e->total += x;
    return e->total;
}
static long fn_peek(Env *e, long x) {
    (void)x;
    return e->total * 100 + e->counter;
}
static long fn_scale(Env *e, long x) {
    e->counter++;
    return x * (e->counter + 1);
}

typedef struct {
    Closure *handlers[6];
    int n;
} Dispatcher;

static void on(Dispatcher *d, Closure *c) {
    check(d->n < 6, "handlers");
    d->handlers[d->n++] = c; /* dispatcher owns the closure */
}

static void fire(Dispatcher *d, long v) {
    printf("fire %ld:", v);
    for (int i = 0; i < d->n; i++)
        printf(" %ld", call(d->handlers[i], v));
    printf("\n");
}

static void clear(Dispatcher *d) {
    for (int i = 0; i < d->n; i++)
        closure_free(d->handlers[i]);
    d->n = 0;
}

int main(void) {
    Env *shared = env_new("acc");
    Env *solo = env_new("scale");
    Dispatcher d = {{0}, 0};

    on(&d, closure_new(fn_add, shared));
    on(&d, closure_new(fn_peek, shared));
    on(&d, closure_new(fn_scale, solo));
    env_release(shared); /* creator's refs dropped; closures keep envs alive */
    env_release(solo);
    printf("envs live: %d closures live: %d\n", live_envs, live_closures);

    fire(&d, 5);
    fire(&d, 7);
    fire(&d, 10);

    /* remove the accumulator: shared env survives through the peeker */
    closure_free(d.handlers[0]);
    d.handlers[0] = d.handlers[1];
    d.handlers[1] = d.handlers[2];
    d.n = 2;
    printf("after removing accumulator: envs live=%d\n", live_envs);
    fire(&d, 1);

    clear(&d);
    printf("envs live: %d closures live: %d\n", live_envs, live_closures);
    check(live_envs == 0 && live_closures == 0, "leak");
    return 0;
}
