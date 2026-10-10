/*
 * title: Bitonic counting network built from atomic toggle balancers
 * topic: concurrency
 * covers: balancer toggles, recursive Bitonic/Merger construction, step property, unique quiescent counter values, contention spreading
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * A balancer has two input wires and two outputs; the i-th token to arrive leaves on the top
 * output if i is even and the bottom if odd. A bitonic counting network of width w routes tokens
 * through log^2 w layers of balancers so that in any quiescent state the per-output-wire counts
 * y_0 .. y_{w-1} obey the step property: y_0 >= y_1 >= ... and y_0 - y_{w-1} <= 1.
 * Attaching a private counter to output wire k that hands out k, k+w, k+2w, ... then gives every
 * token a distinct value, and after quiescence the values are exactly 0 .. n-1. No token ever
 * contends on a single shared counter; contention is spread over the balancers.
 *
 * Construction: Bitonic[w] = Merger[w](Bitonic[w/2], Bitonic[w/2]) and Merger[2k] feeds
 * (even wires of x, odd wires of x') and (odd wires of x, even wires of x') into two Merger[k]
 * and finishes with a layer of balancers pairing their outputs.
 */
enum { MAXW = 8, MAXB = 64, T = 6, PER = 2501 };

typedef struct {
    atomic_uint toggle;
    int next[2]; /* >= 0: next balancer id, < 0: output wire index -(v+1) */
} Balancer;

typedef struct {
    int w;
    int nb;
    Balancer bal[MAXB];
    int first[MAXW];        /* first balancer entered from input wire i, or -(wire+1) */
    atomic_ulong out[MAXW]; /* tokens that left on each output wire */
    /* construction state */
    int end_bal[MAXW];      /* dangling end of each construction wire: balancer id or -1 */
    int end_port[MAXW];
    int start_seen[MAXW];
} Net;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* add a balancer on construction wires p (top) and q (bottom) */
static void add_balancer(Net *n, int p, int q) {
    int id = n->nb++;
    check(id < MAXB, "balancer count");
    atomic_init(&n->bal[id].toggle, 0);
    n->bal[id].next[0] = n->bal[id].next[1] = -1000;
    int wires[2] = {p, q};
    for (int k = 0; k < 2; k++) {
        int wv = wires[k];
        if (n->end_bal[wv] < 0)
            n->first[wv] = id; /* wire was untouched: this is where tokens enter */
        else
            n->bal[n->end_bal[wv]].next[n->end_port[wv]] = id;
        n->end_bal[wv] = id;
        n->end_port[wv] = k;
    }
}

/* Merger: x and y are ordered wire lists of length k; out receives the 2k output wires in order */
static void merger(Net *n, const int *x, const int *y, int k, int *out) {
    if (k == 1) {
        add_balancer(n, x[0], y[0]);
        out[0] = x[0];
        out[1] = y[0];
        return;
    }
    int a_in[MAXW], b_in[MAXW], a_out[MAXW], b_out[MAXW];
    int h = k / 2;
    for (int i = 0; i < h; i++) {
        a_in[i] = x[2 * i];      /* evens of x */
        a_in[h + i] = y[2 * i + 1]; /* odds of y */
        b_in[i] = x[2 * i + 1];  /* odds of x */
        b_in[h + i] = y[2 * i];  /* evens of y */
    }
    /* first halves of each list feed one recursive merger as (x-part, y-part) */
    merger(n, a_in, a_in + h, h, a_out);
    merger(n, b_in, b_in + h, h, b_out);
    for (int i = 0; i < k; i++) {
        add_balancer(n, a_out[i], b_out[i]);
        out[2 * i] = a_out[i];
        out[2 * i + 1] = b_out[i];
    }
}

static void bitonic(Net *n, const int *wires, int w, int *out) {
    if (w == 1) {
        out[0] = wires[0];
        return;
    }
    int lo[MAXW], hi[MAXW];
    bitonic(n, wires, w / 2, lo);
    bitonic(n, wires + w / 2, w / 2, hi);
    merger(n, lo, hi, w / 2, out);
}

static int out_index[MAXW];

static void build(Net *n, int w) {
    n->w = w;
    n->nb = 0;
    for (int i = 0; i < MAXW; i++) {
        n->end_bal[i] = -1;
        n->first[i] = -1;
        atomic_init(&n->out[i], 0ul);
    }
    int wires[MAXW], out[MAXW];
    for (int i = 0; i < w; i++)
        wires[i] = i;
    bitonic(n, wires, w, out);
    for (int pos = 0; pos < w; pos++)
        out_index[out[pos]] = pos; /* construction wire -> output position */
    for (int i = 0; i < w; i++) {
        if (n->first[i] < 0)
            n->first[i] = -(out_index[i] + 1);
        if (n->end_bal[i] >= 0)
            n->bal[n->end_bal[i]].next[n->end_port[i]] = -(out_index[i] + 1);
    }
}

/* push one token in at input wire `in`; returns the output position */
static int traverse(Net *n, int in) {
    int cur = n->first[in];
    while (cur >= 0) {
        unsigned t = atomic_fetch_add_explicit(&n->bal[cur].toggle, 1u, memory_order_relaxed);
        cur = n->bal[cur].next[t & 1u];
    }
    return -cur - 1;
}

static Net net;
static unsigned char *seen;
static long total_tokens;

static void *worker(void *p) {
    int t = (int)(size_t)p;
    for (int i = 0; i < PER; i++) {
        int k = traverse(&net, (t + i / 7) % net.w);
        unsigned long c = atomic_fetch_add_explicit(&net.out[k], 1ul, memory_order_relaxed);
        unsigned long value = (unsigned long)k + c * (unsigned long)net.w;
        check(value < (unsigned long)total_tokens * 2, "value range");
        if (value < (unsigned long)total_tokens)
            seen[value]++; /* transient values above n-1 are possible until quiescence */
    }
    return NULL;
}

int main(void) {
    for (int w = 4; w <= 8; w *= 2) {
        build(&net, w);
        total_tokens = (long)T * PER;
        seen = calloc((size_t)total_tokens, 1);
        check(seen != NULL, "alloc");
        pthread_t th[T];
        for (int i = 0; i < T; i++)
            check(pthread_create(&th[i], NULL, worker, (void *)(size_t)i) == 0, "create");
        for (int i = 0; i < T; i++)
            pthread_join(th[i], NULL);
        unsigned long y[MAXW], sum = 0;
        for (int i = 0; i < w; i++) {
            y[i] = atomic_load(&net.out[i]);
            sum += y[i];
        }
        check(sum == (unsigned long)total_tokens, "all tokens exited");
        for (int i = 0; i + 1 < w; i++)
            check(y[i] >= y[i + 1], "step property: non-increasing");
        check(y[0] - y[w - 1] <= 1, "step property: spread at most 1");
        long dups = 0, holes = 0;
        for (long i = 0; i < total_tokens; i++) {
            if (seen[i] > 1)
                dups++;
            if (seen[i] == 0)
                holes++;
        }
        check(dups == 0 && holes == 0, "values are exactly 0..n-1");
        printf("width %d: balancers=%d tokens=%ld\n", w, net.nb, total_tokens);
        printf("  outputs:");
        for (int i = 0; i < w; i++)
            printf(" %lu", y[i]);
        printf("\n  step property holds, values 0..%ld each handed out once\n", total_tokens - 1);
        free(seen);
    }
    return 0;
}
