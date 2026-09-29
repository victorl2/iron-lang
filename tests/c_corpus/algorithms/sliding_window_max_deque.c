/*
 * title: Sliding window maximum with a monotonic deque
 * topic: algorithms
 * covers: sliding window, monotonic deque, ring buffer of indices, amortised O(n), brute-force verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 909090u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    int *buf;
    int cap, head, len;
} Deque;

static void dq_init(Deque *d, int cap) {
    d->buf = malloc((size_t)cap * sizeof(int));
    if (!d->buf)
        fail("alloc");
    d->cap = cap;
    d->head = 0;
    d->len = 0;
}
static int dq_front(const Deque *d) { return d->buf[d->head]; }
static int dq_back(const Deque *d) { return d->buf[(d->head + d->len - 1) % d->cap]; }
static void dq_push_back(Deque *d, int v) {
    if (d->len == d->cap)
        fail("deque overflow");
    d->buf[(d->head + d->len) % d->cap] = v;
    d->len++;
}
static void dq_pop_back(Deque *d) { d->len--; }
static void dq_pop_front(Deque *d) {
    d->head = (d->head + 1) % d->cap;
    d->len--;
}

/* out[i] = max of a[i..i+k) for i in [0, n-k] ; returns number of windows */
static int window_max(const int *a, int n, int k, int *out, long *ops) {
    Deque d;
    dq_init(&d, k + 1);
    int w = 0;
    for (int i = 0; i < n; i++) {
        while (d.len && a[dq_back(&d)] <= a[i]) {
            dq_pop_back(&d);
            (*ops)++;
        }
        dq_push_back(&d, i);
        if (dq_front(&d) <= i - k)
            dq_pop_front(&d);
        if (i >= k - 1)
            out[w++] = a[dq_front(&d)];
    }
    free(d.buf);
    return w;
}

int main(void) {
    enum { N = 2000 };
    static int a[N], out[N], want[N];
    for (int i = 0; i < N; i++)
        a[i] = (int)(rnd() % 1000);
    int ks[] = {1, 2, 3, 7, 50, 333, 2000};
    for (int t = 0; t < 7; t++) {
        int k = ks[t];
        long ops = 0;
        int w = window_max(a, N, k, out, &ops);
        if (w != N - k + 1)
            fail("window count");
        for (int i = 0; i < w; i++) {
            int m = a[i];
            for (int j = 1; j < k; j++)
                if (a[i + j] > m)
                    m = a[i + j];
            want[i] = m;
            if (out[i] != m)
                fail("window max");
        }
        long s = 0;
        for (int i = 0; i < w; i++)
            s += out[i];
        printf("k=%-5d windows=%-5d sum of maxima=%-8ld pops=%ld (<= n)\n", k, w, s, ops);
        if (ops > N)
            fail("amortised bound");
    }
    int desc[10] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}, o[10];
    long ops = 0;
    int w = window_max(desc, 10, 4, o, &ops);
    printf("descending k=4:");
    for (int i = 0; i < w; i++)
        printf(" %d", o[i]);
    printf("\n");
    return 0;
}
