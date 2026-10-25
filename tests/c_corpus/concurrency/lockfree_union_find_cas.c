/*
 * title: Concurrent union-find with CAS linking and path halving
 * topic: concurrency
 * covers: parent array atomics, CAS root link, path halving, deterministic min-index roots, component counting vs sequential DSU
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * parent[i] == i marks a root. find() walks to the root and compresses with path halving using
 * a CAS on the parent pointer (a failed CAS is harmless: someone else already shortened it).
 * unite(a, b) finds both roots and links the LARGER-index root under the SMALLER with a CAS
 * root -> other. If the CAS fails the root was linked meanwhile and the loop retries with fresh
 * roots.
 *
 * Correctness argument: a parent pointer only ever moves to a smaller index, so there are no
 * cycles and every chain ends at a root. Linking always attaches the larger root to the smaller,
 * so the final root of a component is its minimum element regardless of the order of the unions:
 * the result is fully deterministic and must equal a sequential union-find.
 */
enum { N = 6000, M = 7000, T = 5 };

static atomic_int parent[N];
static int eu[M], ev[M];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int find(int x) {
    for (;;) {
        int p = atomic_load(&parent[x]);
        if (p == x)
            return x;
        int g = atomic_load(&parent[p]);
        if (g != p) /* path halving: point x at its grandparent */
            atomic_compare_exchange_weak(&parent[x], &p, g);
        x = g;
    }
}

static void unite(int a, int b) {
    for (;;) {
        int ra = find(a), rb = find(b);
        if (ra == rb)
            return;
        if (ra < rb) {
            int t = ra;
            ra = rb;
            rb = t;
        } /* now ra > rb: attach ra under rb */
        int expect = ra;
        if (atomic_compare_exchange_strong(&parent[ra], &expect, rb))
            return;
    }
}

static void *worker(void *p) {
    int t = (int)(size_t)p;
    for (int i = t; i < M; i += T)
        unite(eu[i], ev[i]);
    return NULL;
}

/* sequential reference with the same min-root rule */
static int sp[N];
static int sfind(int x) {
    while (sp[x] != x) {
        sp[x] = sp[sp[x]];
        x = sp[x];
    }
    return x;
}

int main(void) {
    unsigned s = 0xdecafu;
    for (int i = 0; i < M; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int a = (int)(s % N);
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int b = (int)(s % N);
        /* mostly local edges so that many mid-size components appear */
        if (i % 3 != 0)
            b = (a + 1 + (int)(s % 40u)) % N;
        eu[i] = a;
        ev[i] = b;
    }
    for (int i = 0; i < N; i++) {
        atomic_init(&parent[i], i);
        sp[i] = i;
    }
    pthread_t th[T];
    for (int i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < M; i++) {
        int a = sfind(eu[i]), b = sfind(ev[i]);
        if (a != b) {
            if (a < b) sp[b] = a; else sp[a] = b;
        }
    }
    int comps = 0, largest = 0;
    static int size[N];
    unsigned long label_sum = 0;
    for (int i = 0; i < N; i++) {
        int r = find(i);
        check(r == sfind(i), "same root as sequential union-find");
        check(r <= i, "root is the minimum element");
        size[r]++;
        label_sum += (unsigned long)r;
    }
    for (int i = 0; i < N; i++)
        if (size[i]) {
            comps++;
            if (size[i] > largest)
                largest = size[i];
        }
    /* size histogram of components */
    int hist[5] = {0};
    for (int i = 0; i < N; i++)
        if (size[i]) {
            int b = size[i] == 1 ? 0 : size[i] <= 10 ? 1 : size[i] <= 100 ? 2 : size[i] <= 1000 ? 3 : 4;
            hist[b]++;
        }
    printf("nodes=%d edges=%d components=%d largest=%d\n", N, M, comps, largest);
    printf("sizes: 1:%d 2-10:%d 11-100:%d 101-1000:%d >1000:%d\n", hist[0], hist[1], hist[2], hist[3], hist[4]);
    printf("sum of root labels=%lu\n", label_sum);
    return 0;
}
