/*
 * title: Recursive parallel merge sort with depth-limited thread spawning
 * topic: concurrency
 * covers: recursive thread creation, spawn depth limit, disjoint ranges, merge, stability check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int key;
    int seq;
} Rec;

typedef struct {
    Rec *a, *tmp;
    int lo, hi; /* [lo, hi) */
    int depth;  /* remaining spawn depth */
} Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int spawned;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void merge(Rec *a, Rec *tmp, int lo, int mid, int hi) {
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi)
        tmp[k++] = (a[j].key < a[i].key) ? a[j++] : a[i++]; /* ties take the left: stable */
    while (i < mid)
        tmp[k++] = a[i++];
    while (j < hi)
        tmp[k++] = a[j++];
    memcpy(a + lo, tmp + lo, sizeof(Rec) * (size_t)(hi - lo));
}

static void *msort(void *p) {
    Job *j = p;
    if (j->hi - j->lo < 2)
        return NULL;
    int mid = j->lo + (j->hi - j->lo) / 2;
    Job left = {j->a, j->tmp, j->lo, mid, j->depth - 1};
    Job right = {j->a, j->tmp, mid, j->hi, j->depth - 1};
    if (j->depth > 0) {
        pthread_t t;
        check(pthread_create(&t, NULL, msort, &left) == 0, "spawn");
        pthread_mutex_lock(&mu);
        spawned++;
        pthread_mutex_unlock(&mu);
        msort(&right);
        pthread_join(t, NULL);
    } else {
        msort(&left);
        msort(&right);
    }
    merge(j->a, j->tmp, j->lo, mid, j->hi);
    return NULL;
}

int main(void) {
    enum { N = 5000 };
    Rec *a = malloc(sizeof(Rec) * N);
    Rec *tmp = malloc(sizeof(Rec) * N);
    Rec *ref = malloc(sizeof(Rec) * N);
    check(a && tmp && ref, "malloc");
    unsigned s = 31337u;
    for (int i = 0; i < N; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        a[i].key = (int)(s % 400u); /* many duplicates */
        a[i].seq = i;
    }
    memcpy(ref, a, sizeof(Rec) * N);

    for (int depth = 0; depth <= 3; depth++) {
        memcpy(a, ref, sizeof(Rec) * N);
        spawned = 0;
        Job root = {a, tmp, 0, N, depth};
        msort(&root);
        for (int i = 1; i < N; i++) {
            check(a[i - 1].key <= a[i].key, "sorted");
            if (a[i - 1].key == a[i].key)
                check(a[i - 1].seq < a[i].seq, "stable");
        }
        check(spawned == (1 << depth) - 1, "spawn count is 2^depth - 1");
        long checksum = 0;
        for (int i = 0; i < N; i++)
            checksum = (checksum * 131 + a[i].key * 7 + a[i].seq) % 1000000007L;
        printf("depth %d: threads spawned=%d checksum=%ld\n", depth, spawned, checksum);
    }
    printf("min=%d max=%d median=%d\n", a[0].key, a[N - 1].key, a[N / 2].key);
    free(a);
    free(tmp);
    free(ref);
    return 0;
}
