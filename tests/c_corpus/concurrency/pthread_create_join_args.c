/*
 * title: Thread create/join with argument structs and return values
 * topic: concurrency
 * covers: pthread_create, pthread_join, argument structs, heap-allocated return values
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int lo, hi; /* inclusive range */
    int id;
} RangeArg;

typedef struct {
    long sum;
    long sumsq;
    int count;
    int id;
} RangeResult;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *range_worker(void *p) {
    RangeArg *a = p;
    RangeResult *r = malloc(sizeof *r);
    if (!r)
        return NULL;
    r->sum = 0;
    r->sumsq = 0;
    r->count = 0;
    r->id = a->id;
    for (int i = a->lo; i <= a->hi; i++) {
        r->sum += i;
        r->sumsq += (long)i * i;
        r->count++;
    }
    return r;
}

static void *scalar_worker(void *p) {
    /* returns a small integer smuggled through the pointer */
    long v = (long)(size_t)p;
    return (void *)(size_t)(v * v + 1);
}

int main(void) {
    enum { T = 6 };
    pthread_t th[T];
    RangeArg args[T];
    int per = 50;
    for (int i = 0; i < T; i++) {
        args[i].lo = i * per + 1;
        args[i].hi = (i + 1) * per;
        args[i].id = i;
        check(pthread_create(&th[i], NULL, range_worker, &args[i]) == 0, "create");
    }
    long total = 0, totalsq = 0;
    int cnt = 0;
    for (int i = 0; i < T; i++) {
        void *ret = NULL;
        check(pthread_join(th[i], &ret) == 0, "join");
        RangeResult *r = ret;
        check(r != NULL, "result");
        check(r->id == i, "result id");
        printf("range %d: [%d..%d] count=%d sum=%ld sumsq=%ld\n", i, args[i].lo, args[i].hi,
               r->count, r->sum, r->sumsq);
        total += r->sum;
        totalsq += r->sumsq;
        cnt += r->count;
        free(r);
    }
    long n = T * per;
    check(cnt == n, "count");
    check(total == n * (n + 1) / 2, "sum formula");
    check(totalsq == n * (n + 1) * (2 * n + 1) / 6, "sumsq formula");
    printf("total=%ld totalsq=%ld count=%d\n", total, totalsq, cnt);

    pthread_t st[4];
    for (long i = 0; i < 4; i++)
        check(pthread_create(&st[i], NULL, scalar_worker, (void *)(size_t)(i + 3)) == 0, "create2");
    for (long i = 0; i < 4; i++) {
        void *ret;
        check(pthread_join(st[i], &ret) == 0, "join2");
        long v = (long)(size_t)ret;
        check(v == (i + 3) * (i + 3) + 1, "scalar");
        printf("scalar %ld -> %ld\n", i + 3, v);
    }
    return 0;
}
