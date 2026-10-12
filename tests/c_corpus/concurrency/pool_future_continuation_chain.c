/*
 * title: Promise continuation chains registered before or after resolution
 * topic: concurrency
 * covers: promises, then() chaining, continuation lists, race between resolve and attach, when_both join
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Promise Promise;
typedef long (*ThenFn)(long);

typedef struct Cont {
    ThenFn fn;
    Promise *out;
    struct Cont *next;
} Cont;

struct Promise {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
    long value;
    Cont *conts; /* pending continuations, most recent first */
};

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Promises are only created by the main thread; the registry lets it free every one. */
static Promise *registry[1024];
static int nreg;

static Promise *promise_new(void) {
    Promise *p = calloc(1, sizeof *p);
    check(p != NULL && nreg < 1024, "alloc");
    registry[nreg++] = p;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    return p;
}

static void free_all(void) {
    for (int i = 0; i < nreg; i++) {
        pthread_mutex_destroy(&registry[i]->mu);
        pthread_cond_destroy(&registry[i]->cv);
        free(registry[i]);
    }
    nreg = 0;
}

static void resolve(Promise *p, long v);

static void run_cont(Cont *c, long v) {
    resolve(c->out, c->fn(v));
    free(c);
}

/* Resolve p and run its continuations on the resolving thread, in registration order. */
static void resolve(Promise *p, long v) {
    pthread_mutex_lock(&p->mu);
    check(!p->done, "resolved twice");
    p->done = 1;
    p->value = v;
    Cont *list = p->conts, *rev = NULL;
    p->conts = NULL;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    while (list) {
        Cont *n = list->next;
        list->next = rev;
        rev = list;
        list = n;
    }
    while (rev) {
        Cont *n = rev->next;
        run_cont(rev, v);
        rev = n;
    }
}

static Promise *then(Promise *p, ThenFn fn) {
    Promise *out = promise_new();
    Cont *c = malloc(sizeof *c);
    check(c != NULL, "alloc");
    c->fn = fn;
    c->out = out;
    pthread_mutex_lock(&p->mu);
    if (!p->done) {
        c->next = p->conts;
        p->conts = c;
        pthread_mutex_unlock(&p->mu);
    } else {
        long v = p->value;
        pthread_mutex_unlock(&p->mu);
        run_cont(c, v);
    }
    return out;
}

static long await(Promise *p) {
    pthread_mutex_lock(&p->mu);
    while (!p->done)
        pthread_cond_wait(&p->cv, &p->mu);
    long v = p->value;
    pthread_mutex_unlock(&p->mu);
    return v;
}

static long add7(long x) { return x + 7; }
static long times3(long x) { return x * 3; }
static long sq_mod(long x) { return x * x % 100003; }
static long minus5(long x) { return x - 5; }
static long xor_bits(long x) { return x ^ 0x5a5a; }

enum { NCHAIN = 24, STAGES = 5 };
static Promise *roots[NCHAIN];
static Promise *tails[NCHAIN];

static void *resolver(void *arg) {
    int lo = (int)(long)arg;
    for (int i = lo; i < NCHAIN; i += 4)
        resolve(roots[i], i * 11 + 1);
    return NULL;
}

/* Two producers feeding a join node: value is only available once both sides resolved. */
typedef struct {
    Promise *a, *b, *out;
} Both;

static void *join_thread(void *arg) {
    Both *j = arg;
    long x = await(j->a);
    long y = await(j->b);
    resolve(j->out, x * 1000 + y);
    return NULL;
}

int main(void) {
    ThenFn stages[STAGES] = {add7, times3, sq_mod, minus5, xor_bits};
    for (int i = 0; i < NCHAIN; i++) {
        roots[i] = promise_new();
        Promise *cur = roots[i];
        /* half of the chains are built before resolution, half racing with it */
        if (i % 2 == 0) {
            for (int s = 0; s < STAGES; s++) {
                cur = then(cur, stages[s]);
            }
        }
        tails[i] = cur;
    }
    pthread_t th[4];
    for (long i = 0; i < 4; i++)
        check(pthread_create(&th[i], NULL, resolver, (void *)i) == 0, "create");
    for (int i = 1; i < NCHAIN; i += 2) {
        Promise *cur = roots[i];
        for (int s = 0; s < STAGES; s++)
            cur = then(cur, stages[s]);
        tails[i] = cur;
    }
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    long sum = 0;
    for (int i = 0; i < NCHAIN; i++) {
        long v = i * 11 + 1;
        for (int s = 0; s < STAGES; s++)
            v = stages[s](v);
        long got = await(tails[i]);
        check(got == v, "chain result");
        sum += got;
        if (i < 6)
            printf("chain %2d -> %ld\n", i, got);
    }
    printf("sum of %d chains: %ld\n", NCHAIN, sum);

    Both j = {tails[0], tails[1], promise_new()};
    pthread_t jt;
    check(pthread_create(&jt, NULL, join_thread, &j) == 0, "create join");
    long joined = await(j.out);
    pthread_join(jt, NULL);
    printf("joined %ld\n", joined);

    /* Continuation attached after resolution runs immediately. */
    Promise *late = then(tails[2], add7);
    check(await(late) == await(tails[2]) + 7, "late continuation");
    printf("late continuation value %ld\n", await(late));
    free_all();
    return 0;
}
