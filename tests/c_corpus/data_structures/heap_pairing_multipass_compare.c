/*
 * title: Pairing heap child-merge strategies compared
 * topic: data_structures
 * covers: pairing heap, two-pass, multi-pass, left-to-right merge, link counting, workload shapes
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 99991;
static unsigned rng(void) {
    rs = rs * 2862933555777941757ull + 3037000493ull;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct PN {
    uint32_t key; /* unique: (value << 12) | sequence */
    struct PN *child, *sib;
} PN;

static long links, maxkids;

static PN *meld(PN *a, PN *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    links++;
    if (b->key < a->key) {
        PN *t = a;
        a = b;
        b = t;
    }
    b->sib = a->child;
    a->child = b;
    return a;
}

typedef PN *(*Merger)(PN *first, int cnt, PN **buf);

static PN *merge_two_pass(PN *first, int cnt, PN **buf) {
    int m = 0;
    while (first) {
        PN *a = first, *b = a->sib;
        first = b ? b->sib : NULL;
        a->sib = NULL;
        if (b)
            b->sib = NULL;
        buf[m++] = meld(a, b);
    }
    PN *r = NULL;
    while (m > 0)
        r = meld(buf[--m], r);
    (void)cnt;
    return r;
}

static PN *merge_multi_pass(PN *first, int cnt, PN **buf) {
    int head = 0, tail = 0, size = cnt;
    /* circular FIFO of size cnt+1 */
    int cap = cnt + 1;
    while (first) {
        PN *nx = first->sib;
        first->sib = NULL;
        buf[tail] = first;
        tail = (tail + 1) % cap;
        first = nx;
    }
    while (size > 1) {
        PN *a = buf[head];
        head = (head + 1) % cap;
        PN *b = buf[head];
        head = (head + 1) % cap;
        buf[tail] = meld(a, b);
        tail = (tail + 1) % cap;
        size--;
    }
    return buf[head];
}

static PN *merge_left_to_right(PN *first, int cnt, PN **buf) {
    PN *r = NULL;
    (void)cnt;
    (void)buf;
    while (first) {
        PN *nx = first->sib;
        first->sib = NULL;
        r = meld(r, first);
        first = nx;
    }
    return r;
}

static PN *merge_right_to_left(PN *first, int cnt, PN **buf) {
    int m = 0;
    while (first) {
        PN *nx = first->sib;
        first->sib = NULL;
        buf[m++] = first;
        first = nx;
    }
    PN *r = NULL;
    while (m > 0)
        r = meld(buf[--m], r);
    (void)cnt;
    return r;
}

static PN *pop(PN *h, uint32_t *out, Merger mg, PN **buf) {
    *out = h->key;
    int cnt = 0;
    for (PN *c = h->child; c; c = c->sib)
        cnt++;
    if (cnt > maxkids)
        maxkids = cnt;
    PN *first = h->child;
    free(h);
    return first ? mg(first, cnt, buf) : NULL;
}

enum { N = 1500 };

static void make_input(uint32_t *v, int shape) {
    for (int i = 0; i < N; i++) {
        uint32_t x;
        switch (shape) {
        case 0: x = rng() % 100000; break;
        case 1: x = (uint32_t)i * 3; break;
        case 2: x = (uint32_t)(N - i) * 3; break;
        default: x = (uint32_t)((i % 50) * 1000 + i / 50); break;
        }
        v[i] = (x << 12) | (uint32_t)i;
    }
}

int main(void) {
    const char *shapes[4] = {"random", "ascending", "descending", "sawtooth"};
    const char *names[4] = {"two_pass", "multi_pass", "left_to_right", "right_to_left"};
    Merger mg[4] = {merge_two_pass, merge_multi_pass, merge_left_to_right, merge_right_to_left};
    static uint32_t input[N], ref[N + 400];
    PN **buf = malloc(sizeof(PN *) * (N + 410));
    for (int s = 0; s < 4; s++) {
        make_input(input, s);
        printf("%s\n", shapes[s]);
        for (int m = 0; m < 4; m++) {
            PN *h = NULL;
            links = 0;
            maxkids = 0;
            for (int i = 0; i < N; i++) {
                PN *n = calloc(1, sizeof(PN));
                n->key = input[i];
                h = meld(h, n);
            }
            long build = links;
            /* warm the tree with a few pops, then interleave inserts */
            int popped = 0;
            uint32_t k;
            int ins = 0;
            uint32_t got[N + 400];
            while (h) {
                h = pop(h, &k, mg[m], buf);
                got[popped++] = k;
                if (ins < 400 && popped % 3 == 0) {
                    PN *n = calloc(1, sizeof(PN));
                    n->key = k + (1u << 12) * 7 + 1u;
                    n->key = (n->key & ~0xFFFu) | (uint32_t)(N + ins);
                    ins++;
                    h = meld(h, n);
                }
            }
            /* compare against the first strategy's output */
            if (m == 0)
                for (int i = 0; i < popped; i++)
                    ref[i] = got[i];
            else
                for (int i = 0; i < popped; i++)
                    check(ref[i] == got[i], "all strategies pop the same sequence");
            check(popped == N + ins, "everything popped");
            printf("  %-14s build_links=%-5ld total_links=%-6ld max_children=%ld\n", names[m], build, links, maxkids);
        }
    }
    free(buf);
    return 0;
}
