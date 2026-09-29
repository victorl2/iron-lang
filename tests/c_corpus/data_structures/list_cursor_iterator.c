/*
 * title: Doubly linked list with editing cursor
 * topic: data_structures
 * covers: list iterator, cursor between elements, insert/remove during iteration, ListIterator semantics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xC0AC29B7C97C50DDULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 16); }

typedef struct N { int v; struct N *prev, *next; } N;
typedef struct { N s; size_t len; } List;
/* A cursor sits BETWEEN elements: `next` is the element that next() would return, prev() returns next->prev.
 * `last` remembers the element returned most recently, which set/remove act on. */
typedef struct { List *l; N *next; N *last; size_t idx; } Cursor;

static void list_init(List *l) { l->s.prev = l->s.next = &l->s; l->len = 0; }
static Cursor cursor_at(List *l, size_t idx) {
    Cursor c = { l, l->s.next, NULL, 0 };
    while (c.idx < idx) { c.next = c.next->next; c.idx++; }
    return c;
}
static int has_next(const Cursor *c) { return c->next != &c->l->s; }
static int has_prev(const Cursor *c) { return c->next->prev != &c->l->s; }
static int cur_next(Cursor *c) { CHECK(has_next(c)); c->last = c->next; c->next = c->next->next; c->idx++; return c->last->v; }
static int cur_prev(Cursor *c) { CHECK(has_prev(c)); c->next = c->next->prev; c->last = c->next; c->idx--; return c->last->v; }
static void cur_insert(Cursor *c, int v) { /* inserted before the cursor: a following prev() returns v */
    N *n = malloc(sizeof *n); CHECK(n);
    n->v = v; n->next = c->next; n->prev = c->next->prev; n->prev->next = n; n->next->prev = n;
    c->l->len++; c->idx++; c->last = NULL;
}
static void cur_remove(Cursor *c) {
    CHECK(c->last);
    N *d = c->last;
    if (d == c->next) c->next = d->next; else c->idx--; /* last came from prev(): cursor already before d's successor */
    d->prev->next = d->next; d->next->prev = d->prev;
    free(d); c->l->len--; c->last = NULL;
}
static void cur_set(Cursor *c, int v) { CHECK(c->last); c->last->v = v; }

#define MAXN 400
int main(void) {
    List l; list_init(&l);
    int m[MAXN]; size_t mn = 0;
    /* model cursor position is an index 0..mn; `mlast` is -1 or the index of the last returned element */
    Cursor c = cursor_at(&l, 0); size_t mi = 0; long mlast = -1;
    long cnt[6] = {0};
    for (int step = 0; step < 30000; step++) {
        unsigned op = rnd() % 100; int v = (int)(rnd() % 1000);
        if (mn > 300) op = 60;
        if (op < 30) { cur_insert(&c, v); memmove(m + mi + 1, m + mi, (mn - mi) * sizeof(int)); m[mi++] = v; mn++; mlast = -1; cnt[0]++; }
        else if (op < 50) { if (mi < mn) { CHECK(cur_next(&c) == m[mi]); mlast = (long)mi; mi++; cnt[1]++; } }
        else if (op < 65) { if (mi > 0) { CHECK(cur_prev(&c) == m[mi - 1]); mi--; mlast = (long)mi; cnt[2]++; } }
        else if (op < 80) {
            if (mlast >= 0) { cur_remove(&c); memmove(m + mlast, m + mlast + 1, (mn - (size_t)mlast - 1) * sizeof(int)); mn--; if ((size_t)mlast < mi) mi--; mlast = -1; cnt[3]++; }
        } else if (op < 90) { if (mlast >= 0) { cur_set(&c, v); m[mlast] = v; cnt[4]++; } }
        else { size_t to = rnd() % (mn + 1); c = cursor_at(&l, to); mi = to; mlast = -1; cnt[5]++; }
        CHECK(l.len == mn && c.idx == mi);
        CHECK(has_next(&c) == (mi < mn) && has_prev(&c) == (mi > 0));
        if (step % 30 == 0) {
            size_t i = 0; for (N *p = l.s.next; p != &l.s; p = p->next, i++) { CHECK(i < mn && p->v == m[i]); CHECK(p->next->prev == p); }
            CHECK(i == mn);
        }
    }
    printf("insert=%ld next=%ld prev=%ld remove=%ld set=%ld seek=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    printf("len=%zu cursor=%zu\n", mn, mi);
    /* in-place filtering: remove all multiples of 3 with a single forward pass */
    Cursor it = cursor_at(&l, 0); long removed = 0;
    while (has_next(&it)) { int x = cur_next(&it); if (x % 3 == 0) { cur_remove(&it); removed++; } }
    size_t k = 0; for (size_t i = 0; i < mn; i++) if (m[i] % 3) m[k++] = m[i];
    CHECK(k == l.len && (size_t)removed == mn - k);
    /* duplicate every element in place using insert-after-next */
    Cursor dup = cursor_at(&l, 0);
    while (has_next(&dup)) { int x = cur_next(&dup); cur_insert(&dup, x); }
    CHECK(l.len == 2 * k);
    size_t i = 0; for (N *p = l.s.next; p != &l.s; p = p->next, i++) CHECK(p->v == m[i / 2]);
    printf("filtered_out=%ld kept=%zu after_duplication=%zu\n", removed, k, l.len);
    printf("first:"); { int shown = 0; for (N *p = l.s.next; p != &l.s && shown < 8; p = p->next, shown++) printf(" %d", p->v); }
    printf("\n");
    while (l.len) { N *d = l.s.next; d->prev->next = d->next; d->next->prev = d->prev; free(d); l.len--; }
    return 0;
}
