/*
 * title: Circular doubly linked list with sentinel node
 * topic: data_structures
 * covers: doubly linked list, sentinel, circular links, bidirectional iteration, node handles
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x123456789ABCDEFULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 24); }

typedef struct DNode { struct DNode *prev, *next; int val; } DNode;
typedef struct { DNode sent; size_t len; } DList;

static void dl_init(DList *l) { l->sent.prev = l->sent.next = &l->sent; l->len = 0; }
static DNode *dl_insert_before(DList *l, DNode *pos, int v) {
    DNode *n = malloc(sizeof *n); CHECK(n);
    n->val = v; n->next = pos; n->prev = pos->prev;
    pos->prev->next = n; pos->prev = n; l->len++;
    return n;
}
static int dl_unlink(DList *l, DNode *n) {
    CHECK(n != &l->sent);
    int v = n->val;
    n->prev->next = n->next; n->next->prev = n->prev;
    free(n); l->len--;
    return v;
}
static DNode *dl_front(DList *l) { return l->sent.next; }
static DNode *dl_back(DList *l) { return l->sent.prev; }
static void dl_clear(DList *l) { while (l->len) dl_unlink(l, dl_front(l)); }

/* Rotating: move front node to the back in O(1) without allocation. */
static void dl_rotate_left(DList *l) {
    if (l->len < 2) return;
    DNode *n = dl_front(l);
    n->prev->next = n->next; n->next->prev = n->prev;
    n->prev = l->sent.prev; n->next = &l->sent;
    l->sent.prev->next = n; l->sent.prev = n;
}

#define MAXN 300
static int model[MAXN];
static size_t mlen;

static void verify(DList *l) {
    CHECK(l->len == mlen);
    size_t i = 0;
    for (DNode *p = dl_front(l); p != &l->sent; p = p->next, i++) {
        CHECK(i < mlen && p->val == model[i]);
        CHECK(p->next->prev == p && p->prev->next == p);
    }
    CHECK(i == mlen);
    i = mlen;
    for (DNode *p = dl_back(l); p != &l->sent; p = p->prev) { CHECK(i > 0); CHECK(p->val == model[--i]); }
    CHECK(i == 0);
}

int main(void) {
    DList l; dl_init(&l);
    long cnt[6] = {0};
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        int v = (int)(rnd() % 500);
        if (mlen > 250) op = 90;
        if (op < 20) { dl_insert_before(&l, dl_front(&l), v); for (size_t i = mlen; i > 0; i--) model[i] = model[i-1]; model[0] = v; mlen++; cnt[0]++; }
        else if (op < 40) { dl_insert_before(&l, &l.sent, v); model[mlen++] = v; cnt[1]++; }
        else if (op < 60) {
            size_t k = rnd() % (mlen + 1);
            DNode *p = dl_front(&l);
            for (size_t i = 0; i < k; i++) p = p->next;
            dl_insert_before(&l, p, v);
            for (size_t i = mlen; i > k; i--) model[i] = model[i-1];
            model[k] = v; mlen++; cnt[2]++;
        } else if (op < 72 && mlen) { CHECK(dl_unlink(&l, dl_front(&l)) == model[0]); for (size_t i = 1; i < mlen; i++) model[i-1] = model[i]; mlen--; cnt[3]++; }
        else if (op < 82 && mlen) { CHECK(dl_unlink(&l, dl_back(&l)) == model[mlen-1]); mlen--; cnt[3]++; }
        else if (op < 90 && mlen) {
            size_t k = rnd() % mlen;
            /* walk from the nearer end */
            DNode *p;
            if (k < mlen / 2) { p = dl_front(&l); for (size_t i = 0; i < k; i++) p = p->next; }
            else { p = dl_back(&l); for (size_t i = mlen - 1; i > k; i--) p = p->prev; }
            CHECK(dl_unlink(&l, p) == model[k]);
            for (size_t i = k + 1; i < mlen; i++) model[i-1] = model[i];
            mlen--; cnt[4]++;
        } else if (mlen > 1) {
            dl_rotate_left(&l);
            int f = model[0];
            for (size_t i = 1; i < mlen; i++) model[i-1] = model[i];
            model[mlen-1] = f; cnt[5]++;
        }
        verify(&l);
    }
    printf("front=%ld back=%ld middle=%ld pop=%ld erase=%ld rotate=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    printf("len=%zu forward:", l.len);
    int shown = 0;
    for (DNode *p = dl_front(&l); p != &l.sent && shown < 6; p = p->next, shown++) printf(" %d", p->val);
    printf("\nbackward:");
    shown = 0;
    for (DNode *p = dl_back(&l); p != &l.sent && shown < 6; p = p->prev, shown++) printf(" %d", p->val);
    printf("\n");
    dl_clear(&l);
    CHECK(l.sent.next == &l.sent && l.sent.prev == &l.sent);
    return 0;
}
