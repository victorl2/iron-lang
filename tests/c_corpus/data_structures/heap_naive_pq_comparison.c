/*
 * title: Priority queue implementations compared by operation counts
 * topic: data_structures
 * covers: unsorted array PQ, sorted array PQ, sorted linked list PQ, binary heap PQ, function pointer interface, workload profiles
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xFA57ull;
static unsigned rng(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long steps; /* comparisons plus element moves */

typedef struct PQ PQ;
struct PQ {
    const char *name;
    void (*push)(PQ *, int);
    int (*pop)(PQ *);
    int (*size)(PQ *);
    void (*destroy)(PQ *);
};

/* ---- unsorted array: O(1) push, O(n) pop ---- */
typedef struct {
    PQ base;
    int *a, n, cap;
} UArr;
static void u_push(PQ *q, int v) {
    UArr *u = (UArr *)q;
    if (u->n == u->cap) {
        u->cap = u->cap ? 2 * u->cap : 16;
        u->a = realloc(u->a, sizeof(int) * (size_t)u->cap);
    }
    u->a[u->n++] = v;
    steps++;
}
static int u_pop(PQ *q) {
    UArr *u = (UArr *)q;
    int bi = 0;
    for (int i = 1; i < u->n; i++) {
        steps++;
        if (u->a[i] < u->a[bi])
            bi = i;
    }
    int v = u->a[bi];
    u->a[bi] = u->a[--u->n];
    steps++;
    return v;
}
static int u_size(PQ *q) { return ((UArr *)q)->n; }
static void u_destroy(PQ *q) {
    free(((UArr *)q)->a);
    free(q);
}

/* ---- sorted array (descending): binary-search insert with memmove, O(1) pop ---- */
static void s_push(PQ *q, int v) {
    UArr *u = (UArr *)q;
    if (u->n == u->cap) {
        u->cap = u->cap ? 2 * u->cap : 16;
        u->a = realloc(u->a, sizeof(int) * (size_t)u->cap);
    }
    int lo = 0, hi = u->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        steps++;
        if (u->a[mid] > v)
            lo = mid + 1;
        else
            hi = mid;
    }
    memmove(u->a + lo + 1, u->a + lo, sizeof(int) * (size_t)(u->n - lo));
    steps += u->n - lo;
    u->a[lo] = v;
    u->n++;
}
static int s_pop(PQ *q) {
    UArr *u = (UArr *)q;
    steps++;
    return u->a[--u->n];
}

/* ---- sorted linked list (ascending): O(n) push, O(1) pop ---- */
typedef struct LNode {
    int v;
    struct LNode *next;
} LNode;
typedef struct {
    PQ base;
    LNode *head;
    int n;
} LList;
static void l_push(PQ *q, int v) {
    LList *l = (LList *)q;
    LNode *nd = malloc(sizeof(LNode)), **pp = &l->head;
    nd->v = v;
    while (*pp) {
        steps++;
        if ((*pp)->v > v)
            break;
        pp = &(*pp)->next;
    }
    nd->next = *pp;
    *pp = nd;
    l->n++;
}
static int l_pop(PQ *q) {
    LList *l = (LList *)q;
    LNode *h = l->head;
    int v = h->v;
    l->head = h->next;
    free(h);
    l->n--;
    steps++;
    return v;
}
static int l_size(PQ *q) { return ((LList *)q)->n; }
static void l_destroy(PQ *q) {
    LList *l = (LList *)q;
    while (l->head) {
        LNode *n = l->head->next;
        free(l->head);
        l->head = n;
    }
    free(q);
}

/* ---- binary heap ---- */
static void h_push(PQ *q, int v) {
    UArr *u = (UArr *)q;
    if (u->n == u->cap) {
        u->cap = u->cap ? 2 * u->cap : 16;
        u->a = realloc(u->a, sizeof(int) * (size_t)u->cap);
    }
    int i = u->n++;
    while (i > 0) {
        steps++;
        if (u->a[(i - 1) / 2] <= v)
            break;
        u->a[i] = u->a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    u->a[i] = v;
}
static int h_pop(PQ *q) {
    UArr *u = (UArr *)q;
    int top = u->a[0], x = u->a[--u->n], i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= u->n)
            break;
        if (c + 1 < u->n) {
            steps++;
            if (u->a[c + 1] < u->a[c])
                c++;
        }
        steps++;
        if (u->a[c] >= x)
            break;
        u->a[i] = u->a[c];
        i = c;
    }
    if (u->n > 0)
        u->a[i] = x;
    return top;
}

static PQ *make(int kind) {
    if (kind == 2) {
        LList *l = calloc(1, sizeof(LList));
        l->base = (PQ){"sorted list", l_push, l_pop, l_size, l_destroy};
        return &l->base;
    }
    UArr *u = calloc(1, sizeof(UArr));
    if (kind == 0)
        u->base = (PQ){"unsorted array", u_push, u_pop, u_size, u_destroy};
    else if (kind == 1)
        u->base = (PQ){"sorted array", s_push, s_pop, u_size, u_destroy};
    else
        u->base = (PQ){"binary heap", h_push, h_pop, u_size, u_destroy};
    return &u->base;
}

typedef struct {
    int is_push, val;
} Op;

int main(void) {
    enum { NOPS = 6000 };
    static Op script[NOPS];
    const char *wl_name[3] = {"fill then drain", "steady state ~800", "bursty"};
    for (int wl = 0; wl < 3; wl++) {
        int n = 0, ns = 0;
        while (ns < NOPS) {
            int push;
            if (wl == 0)
                push = ns < NOPS / 2;
            else if (wl == 1)
                push = n < 800 || rng() % 2;
            else
                push = (ns / 400) % 2 == 0;
            if (n == 0)
                push = 1;
            script[ns].is_push = push;
            script[ns].val = (int)(rng() % 100000);
            n += push ? 1 : -1;
            ns++;
        }
        printf("%s\n", wl_name[wl]);
        long ref_sum = 0;
        unsigned long long ref_hash = 0;
        for (int kind = 0; kind < 4; kind++) {
            PQ *q = make(kind);
            steps = 0;
            long sum = 0;
            unsigned long long hash = 1469598103934665603ull;
            for (int i = 0; i < NOPS; i++) {
                if (script[i].is_push)
                    q->push(q, script[i].val);
                else {
                    int v = q->pop(q);
                    sum += v;
                    hash = (hash ^ (unsigned)v) * 1099511628211ull;
                }
            }
            long final_size = q->size(q);
            if (kind == 0) {
                ref_sum = sum;
                ref_hash = hash;
            } else {
                check(sum == ref_sum && hash == ref_hash, "all four queues return the same pop stream");
            }
            printf("  %-15s steps=%-9ld final_size=%ld popsum=%ld\n", q->name, steps, final_size, sum);
            q->destroy(q);
        }
    }
    return 0;
}
