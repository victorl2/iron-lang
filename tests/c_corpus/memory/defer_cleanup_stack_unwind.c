/*
 * title: Defer stack with LIFO cleanup and error unwinding
 * topic: memory
 * covers: cleanup stack, deferred actions, LIFO unwinding, cancel/commit of registered cleanups, function pointers
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void (*fn)(void *);
    void *arg;
    const char *what;
    int active;
} Action;

typedef struct {
    Action acts[16];
    int n;
} Defer;

static char trace[256];
static int live_blocks;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void defer_push(Defer *d, void (*fn)(void *), void *arg, const char *what) {
    check(d->n < 16, "defer overflow");
    d->acts[d->n].fn = fn;
    d->acts[d->n].arg = arg;
    d->acts[d->n].what = what;
    d->acts[d->n].active = 1;
    d->n++;
}

static void defer_cancel(Defer *d, void *arg) {
    for (int i = d->n - 1; i >= 0; i--)
        if (d->acts[i].arg == arg && d->acts[i].active) {
            d->acts[i].active = 0;
            return;
        }
    check(0, "cancel of unknown action");
}

/* Run all still-active actions in reverse registration order. */
static void defer_unwind(Defer *d) {
    while (d->n > 0) {
        Action *a = &d->acts[--d->n];
        if (a->active) {
            strcat(trace, a->what);
            strcat(trace, " ");
            a->fn(a->arg);
        }
    }
}

static void free_block(void *p) {
    free(p);
    live_blocks--;
}

static void *blk(size_t n) {
    void *p = malloc(n);
    check(p != NULL, "alloc");
    live_blocks++;
    return p;
}

static int counter_open;
static void close_thing(void *p) {
    (void)p;
    counter_open--;
}

/* Build a "document"; fail_step selects where an error occurs (0 = never).
 * On success ownership of the result buffer is handed out (its cleanup is cancelled). */
static char *build(int fail_step, int *ok) {
    Defer d = {.n = 0};
    *ok = 0;
    char *result = NULL;

    counter_open++;
    defer_push(&d, close_thing, &counter_open, "close");

    char *header = blk(16);
    defer_push(&d, free_block, header, "header");
    if (fail_step == 1)
        goto out;
    strcpy(header, "HDR");

    char *body = blk(64);
    defer_push(&d, free_block, body, "body");
    if (fail_step == 2)
        goto out;
    snprintf(body, 64, "%s|body", header);

    result = blk(128);
    defer_push(&d, free_block, result, "result");
    if (fail_step == 3) {
        goto out;
    }
    snprintf(result, 128, "%s|footer", body);

    /* success: the caller owns result, so cancel its cleanup */
    defer_cancel(&d, result);
    *ok = 1;
out:
    defer_unwind(&d);
    return *ok ? result : NULL;
}

int main(void) {
    for (int step = 0; step <= 3; step++) {
        int ok;
        trace[0] = 0;
        char *r = build(step, &ok);
        printf("fail_step=%d ok=%d result=%s unwound: %s\n", step, ok, r ? r : "(none)", trace);
        check(ok == (step == 0), "outcome");
        check(counter_open == 0, "handle closed");
        if (r) {
            check(live_blocks == 1, "only result alive");
            free_block(r);
        }
        check(live_blocks == 0, "leak");
    }

    /* nested unwinding: an inner Defer runs before the outer one */
    Defer outer = {.n = 0};
    char *o1 = blk(4), *o2 = blk(4);
    defer_push(&outer, free_block, o1, "outer1");
    defer_push(&outer, free_block, o2, "outer2");
    {
        Defer inner = {.n = 0};
        char *i1 = blk(4);
        defer_push(&inner, free_block, i1, "inner1");
        trace[0] = 0;
        defer_unwind(&inner);
        printf("inner: %s\n", trace);
    }
    trace[0] = 0;
    defer_unwind(&outer);
    printf("outer: %s\n", trace);
    check(live_blocks == 0, "final leak");
    return 0;
}
