/*
 * title: XOR linked list with two-way traversal
 * topic: data_structures
 * covers: xor linked list, uintptr_t pointer arithmetic, bidirectional traversal, reverse in O(1)
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xDEADBEEFCAFEF00DULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 18); }

typedef struct XNode { int val; uintptr_t link; } XNode; /* link = prev ^ next */
typedef struct { XNode *head, *tail; size_t len; } XList;

static uintptr_t P(const XNode *n) { return (uintptr_t)n; }
static XNode *N(uintptr_t u) { return (XNode *)u; }

static void push_back(XList *l, int v) {
    XNode *n = malloc(sizeof *n); CHECK(n);
    n->val = v; n->link = P(l->tail) ^ P(NULL);
    if (l->tail) l->tail->link ^= P(NULL) ^ P(n); else l->head = n;
    l->tail = n; l->len++;
}
static void push_front(XList *l, int v) {
    XNode *n = malloc(sizeof *n); CHECK(n);
    n->val = v; n->link = P(NULL) ^ P(l->head);
    if (l->head) l->head->link ^= P(NULL) ^ P(n); else l->tail = n;
    l->head = n; l->len++;
}
static int pop_front(XList *l) {
    CHECK(l->head);
    XNode *n = l->head; int v = n->val;
    XNode *nx = N(n->link ^ P(NULL));
    if (nx) nx->link ^= P(n) ^ P(NULL); else l->tail = NULL;
    l->head = nx; l->len--; free(n);
    return v;
}
static int pop_back(XList *l) {
    CHECK(l->tail);
    XNode *n = l->tail; int v = n->val;
    XNode *pv = N(n->link ^ P(NULL));
    if (pv) pv->link ^= P(n) ^ P(NULL); else l->head = NULL;
    l->tail = pv; l->len--; free(n);
    return v;
}
/* O(1) reversal: swap the ends. */
static void reverse(XList *l) { XNode *t = l->head; l->head = l->tail; l->tail = t; }

/* insert after the k-th node (0-based) walking from the head */
static void insert_after_index(XList *l, size_t k, int v) {
    CHECK(k < l->len);
    XNode *prev = NULL, *cur = l->head;
    for (size_t i = 0; i < k; i++) { XNode *nx = N(cur->link ^ P(prev)); prev = cur; cur = nx; }
    XNode *nx = N(cur->link ^ P(prev));
    if (!nx) { push_back(l, v); return; }
    XNode *n = malloc(sizeof *n); CHECK(n);
    n->val = v; n->link = P(cur) ^ P(nx);
    cur->link ^= P(nx) ^ P(n);
    nx->link ^= P(cur) ^ P(n);
    l->len++;
}
static void to_array(const XList *l, int *out, int forward) {
    XNode *prev = NULL, *cur = forward ? l->head : l->tail;
    size_t i = forward ? 0 : l->len;
    while (cur) {
        if (forward) out[i++] = cur->val; else out[--i] = cur->val;
        XNode *nx = N(cur->link ^ P(prev)); prev = cur; cur = nx;
    }
}

#define MAXN 600
static int model[MAXN];
static size_t mlen;

static void model_reverse(void) { for (size_t i = 0, j = mlen; i + 1 < j; i++, j--) { int t = model[i]; model[i] = model[j-1]; model[j-1] = t; } }

int main(void) {
    XList l = {0};
    long cnt[6] = {0};
    int buf[MAXN];
    for (int step = 0; step < 4000; step++) {
        unsigned op = rnd() % 100;
        int v = (int)(rnd() % 10000);
        if (mlen > 500) op = 60;
        if (op < 20) { push_back(&l, v); model[mlen++] = v; cnt[0]++; }
        else if (op < 40) { push_front(&l, v); memmove(model + 1, model, mlen * sizeof(int)); model[0] = v; mlen++; cnt[1]++; }
        else if (op < 55 && mlen) { CHECK(pop_front(&l) == model[0]); memmove(model, model + 1, --mlen * sizeof(int)); cnt[2]++; }
        else if (op < 70 && mlen) { CHECK(pop_back(&l) == model[--mlen]); cnt[3]++; }
        else if (op < 85 && mlen) {
            size_t k = rnd() % mlen;
            insert_after_index(&l, k, v);
            memmove(model + k + 2, model + k + 1, (mlen - k - 1) * sizeof(int)); model[k + 1] = v; mlen++; cnt[4]++;
        } else if (mlen) { reverse(&l); model_reverse(); cnt[5]++; }
        CHECK(l.len == mlen);
        if (step % 25 == 0 || mlen < 4) {
            to_array(&l, buf, 1); CHECK(memcmp(buf, model, mlen * sizeof(int)) == 0);
            to_array(&l, buf, 0); CHECK(memcmp(buf, model, mlen * sizeof(int)) == 0);
        }
    }
    printf("push_back=%ld push_front=%ld pop_front=%ld pop_back=%ld insert_after=%ld reverse=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    printf("len=%zu head=%d tail=%d node_size_words=%zu\n", l.len, l.head ? l.head->val : -1, l.tail ? l.tail->val : -1, sizeof(XNode) / sizeof(uintptr_t));
    to_array(&l, buf, 1);
    long sum = 0; for (size_t i = 0; i < l.len; i++) sum += (long)buf[i] * (long)(i + 1);
    printf("weighted_sum=%ld\n", sum);
    while (l.len) pop_front(&l);
    CHECK(!l.head && !l.tail);
    return 0;
}
