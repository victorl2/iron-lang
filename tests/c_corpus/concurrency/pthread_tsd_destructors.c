/*
 * title: Thread-specific data with destructors
 * topic: concurrency
 * covers: pthread_key_create, pthread_setspecific, pthread_getspecific, key destructors
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 6 };

typedef struct {
    int owner;
    int uses;
    long total;
} Ctx;

static pthread_key_t key;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int destroyed_count;
static long destroyed_uses;
static long destroyed_total;
static int destroyed_owner_mask;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void ctx_destroy(void *p) {
    Ctx *c = p;
    pthread_mutex_lock(&mu);
    destroyed_count++;
    destroyed_uses += c->uses;
    destroyed_total += c->total;
    destroyed_owner_mask |= 1 << c->owner;
    pthread_mutex_unlock(&mu);
    free(c);
}

static Ctx *ctx_get(int owner) {
    Ctx *c = pthread_getspecific(key);
    if (!c) {
        c = calloc(1, sizeof *c);
        c->owner = owner;
        check(pthread_setspecific(key, c) == 0, "setspecific");
    }
    return c;
}

static void add_sample(int owner, int v) {
    Ctx *c = ctx_get(owner);
    c->uses++;
    c->total += v;
}

static void *worker(void *p) {
    int id = (int)(size_t)p;
    /* nested helper calls all reach the same per-thread context without passing it */
    for (int i = 1; i <= 10 * (id + 1); i++)
        add_sample(id, i);
    Ctx *c = pthread_getspecific(key);
    check(c != NULL && c->owner == id, "own context");
    return NULL;
}

int main(void) {
    check(pthread_key_create(&key, ctx_destroy) == 0, "key create");
    check(pthread_getspecific(key) == NULL, "main has no value yet");
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    /* destructors ran at thread exit, before join returned */
    long expect_uses = 0, expect_total = 0;
    for (int id = 0; id < T; id++) {
        int n = 10 * (id + 1);
        expect_uses += n;
        expect_total += (long)n * (n + 1) / 2;
    }
    check(destroyed_count == T, "one destructor per thread");
    check(destroyed_uses == expect_uses, "uses");
    check(destroyed_total == expect_total, "total");
    check(destroyed_owner_mask == (1 << T) - 1, "every owner destroyed");
    printf("destructors run=%d\n", destroyed_count);
    printf("uses=%ld total=%ld\n", destroyed_uses, destroyed_total);

    /* main thread: value set, key deleted, destructor is NOT called for main */
    add_sample(7, 5);
    Ctx *mine = pthread_getspecific(key);
    check(mine != NULL && mine->uses == 1, "main context");
    pthread_setspecific(key, NULL);
    ctx_destroy(mine);
    check(destroyed_count == T + 1, "manual destroy");
    printf("main context freed manually, count=%d\n", destroyed_count);
    check(pthread_key_delete(key) == 0, "key delete");
    return 0;
}
