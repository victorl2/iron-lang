/*
 * title: Thread attributes: stack size set and read back
 * topic: concurrency
 * covers: pthread_attr_setstacksize, pthread_attr_getstacksize, deep recursion on custom stack, detachstate
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int depth;
    long result;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* each frame holds a little state so depth translates into real stack use */
static long descend(int depth, unsigned salt) {
    volatile unsigned pad[8];
    for (int i = 0; i < 8; i++)
        pad[i] = salt + (unsigned)i;
    if (depth == 0)
        return pad[3];
    long r = descend(depth - 1, salt + 1);
    return r + (long)(pad[depth % 8] & 1u);
}

static long descend_ref(int depth, unsigned salt) {
    /* iterative equivalent: value at the bottom is salt+depth+3, plus parity bits on the way up */
    long r = (long)(salt + (unsigned)depth + 3u);
    for (int d = 1; d <= depth; d++) {
        unsigned s = salt + (unsigned)(depth - d);
        r += (long)((s + (unsigned)(d % 8)) & 1u);
    }
    return r;
}

static void *worker(void *p) {
    Arg *a = p;
    a->result = descend(a->depth, 5u);
    return NULL;
}

int main(void) {
    const size_t sizes[3] = {512u * 1024u, 1024u * 1024u, 2048u * 1024u};
    const int depths[3] = {500, 1000, 2000}; /* about 100 bytes per frame, fits comfortably */
    for (int i = 0; i < 3; i++) {
        pthread_attr_t at;
        check(pthread_attr_init(&at) == 0, "attr init");
        check(pthread_attr_setstacksize(&at, sizes[i]) == 0, "setstacksize");
        size_t got = 0;
        check(pthread_attr_getstacksize(&at, &got) == 0, "getstacksize");
        check(got == sizes[i], "stack size reads back exactly");
        int ds = -1;
        pthread_attr_getdetachstate(&at, &ds);
        check(ds == PTHREAD_CREATE_JOINABLE, "default is joinable");

        Arg a = {depths[i], 0};
        pthread_t t;
        check(pthread_create(&t, &at, worker, &a) == 0, "create");
        pthread_join(t, NULL);
        check(a.result == descend_ref(depths[i], 5u), "recursion result");
        printf("stack %zu KiB: depth %d -> %ld\n", got / 1024, depths[i], a.result);
        pthread_attr_destroy(&at);
    }

    /* an unreasonably small stack must be rejected by setstacksize (EINVAL) */
    pthread_attr_t bad;
    pthread_attr_init(&bad);
    int rc = pthread_attr_setstacksize(&bad, 16);
    printf("tiny stack rejected: %s\n", rc != 0 ? "yes" : "no");
    check(rc != 0, "tiny stack rejected");
    size_t still = 0;
    pthread_attr_getstacksize(&bad, &still);
    check(still >= 16 * 1024, "attr unchanged after rejected set");
    pthread_attr_destroy(&bad);
    return 0;
}
