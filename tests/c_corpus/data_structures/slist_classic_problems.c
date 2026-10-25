/*
 * title: Classic singly linked list interview problems
 * topic: data_structures
 * covers: nth from end, middle, palindrome, intersection, rotate, odd-even, add two numbers, remove duplicates
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x082EFA98EC4E6C89ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 31); }

typedef struct Node { int v; struct Node *next; } Node;
static Node *mk(int v, Node *n) { Node *x = malloc(sizeof *x); CHECK(x); x->v = v; x->next = n; return x; }
static Node *from(const int *a, int n) { Node *h = NULL, **pp = &h; for (int i = 0; i < n; i++) { *pp = mk(a[i], NULL); pp = &(*pp)->next; } return h; }
static int to(const Node *h, int *a) { int n = 0; for (; h; h = h->next) a[n++] = h->v; return n; }
static void destroy(Node *h) { while (h) { Node *n = h->next; free(h); h = n; } }
static Node *rev(Node *h) { Node *p = NULL; while (h) { Node *n = h->next; h->next = p; p = h; h = n; } return p; }

static Node *remove_nth_from_end(Node *h, int n) {
    Node dummy = { 0, h }, *fast = &dummy, *slow = &dummy;
    for (int i = 0; i < n && fast; i++) fast = fast->next;
    if (!fast) return h;
    while (fast->next) { fast = fast->next; slow = slow->next; }
    Node *d = slow->next; slow->next = d->next; free(d);
    return dummy.next;
}
static Node *middle(Node *h) { Node *s = h, *f = h; while (f && f->next) { s = s->next; f = f->next->next; } return s; }
static int is_palindrome(Node *h) {
    if (!h || !h->next) return 1;
    Node *mid = h, *fast = h; /* first middle */
    while (fast->next && fast->next->next) { mid = mid->next; fast = fast->next->next; }
    Node *second = rev(mid->next);
    mid->next = NULL;
    Node *a = h, *b = second; int ok = 1;
    while (b) { if (a->v != b->v) { ok = 0; break; } a = a->next; b = b->next; }
    mid->next = rev(second); /* restore */
    return ok;
}
static Node *intersection(Node *a, Node *b) {
    Node *p = a, *q = b;
    while (p != q) { p = p ? p->next : b; q = q ? q->next : a; }
    return p;
}
static Node *rotate_right(Node *h, int k) {
    if (!h || !h->next) return h;
    int n = 1; Node *t = h; while (t->next) { t = t->next; n++; }
    k %= n; if (!k) return h;
    Node *p = h; for (int i = 0; i < n - k - 1; i++) p = p->next;
    Node *nh = p->next; p->next = NULL; t->next = h;
    return nh;
}
static Node *odd_even(Node *h) {
    if (!h) return h;
    Node *odd = h, *even = h->next, *even_head = even;
    while (even && even->next) { odd->next = even->next; odd = odd->next; even->next = odd->next; even = even->next; }
    odd->next = even_head;
    return h;
}
/* digits stored least significant first */
static Node *add_numbers(const Node *a, const Node *b) {
    Node *h = NULL, **pp = &h; int carry = 0;
    while (a || b || carry) {
        int s = carry + (a ? a->v : 0) + (b ? b->v : 0);
        *pp = mk(s % 10, NULL); pp = &(*pp)->next; carry = s / 10;
        if (a) a = a->next;
        if (b) b = b->next;
    }
    return h;
}
static Node *remove_all_duplicates(Node *h) { /* drop every value that appears more than once (sorted input) */
    Node dummy = { 0, h }, *p = &dummy;
    while (p->next) {
        Node *c = p->next;
        if (c->next && c->next->v == c->v) {
            int v = c->v;
            while (p->next && p->next->v == v) { Node *d = p->next; p->next = d->next; free(d); }
        } else p = c;
    }
    return dummy.next;
}
static void show(const char *label, const Node *h) { printf("%-22s:", label); for (; h; h = h->next) printf(" %d", h->v); printf("\n"); }

int main(void) {
    static const int base[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    Node *h = from(base, 8);
    h = remove_nth_from_end(h, 3); show("remove 3rd from end", h);
    printf("middle: %d\n", middle(h)->v);
    h = rotate_right(h, 10); show("rotate right 10", h);
    h = odd_even(h); show("odd-even regroup", h);
    destroy(h);

    static const int pal[] = { 1, 2, 3, 2, 1 }, nop[] = { 1, 2, 3, 4 };
    Node *p1 = from(pal, 5), *p2 = from(nop, 4);
    printf("palindrome %d %d\n", is_palindrome(p1), is_palindrome(p2));
    destroy(p1); destroy(p2);

    static const int d1[] = { 9, 9, 9, 1 }, d2[] = { 2, 1 }; /* 1999 + 12 */
    Node *s1 = from(d1, 4), *s2 = from(d2, 2), *sum = add_numbers(s1, s2);
    show("1999 + 12 (lsd first)", sum);
    destroy(s1); destroy(s2); destroy(sum);

    static const int dup[] = { 1, 1, 2, 3, 3, 3, 4, 5, 5 };
    Node *dd = remove_all_duplicates(from(dup, 9)); show("strictly unique", dd); destroy(dd);

    long checks = 0;
    for (int t = 0; t < 500; t++) {
        int a[40], n = (int)(rnd() % 30), out[40];
        for (int i = 0; i < n; i++) a[i] = (int)(rnd() % 4);
        Node *l = from(a, n);
        int k = 1 + (int)(rnd() % 35);
        /* nth from end */
        l = remove_nth_from_end(l, k);
        int exp[40], en = 0;
        for (int i = 0; i < n; i++) if (!(k <= n && i == n - k)) exp[en++] = a[i];
        CHECK(to(l, out) == en && memcmp(out, exp, (size_t)en * sizeof(int)) == 0); checks++;
        /* palindrome vs array test, and that the list was restored */
        int pal_ok = 1; for (int i = 0; i < en / 2; i++) if (exp[i] != exp[en - 1 - i]) pal_ok = 0;
        CHECK(is_palindrome(l) == pal_ok); CHECK(to(l, out) == en && memcmp(out, exp, (size_t)en * sizeof(int)) == 0); checks++;
        /* rotate */
        int r = (int)(rnd() % 50);
        l = rotate_right(l, r);
        for (int i = 0; i < en; i++) { int idx = ((i - r) % en + en) % en; out[i] = exp[idx]; }
        int got[40]; CHECK(to(l, got) == en && (en == 0 || memcmp(got, out, (size_t)en * sizeof(int)) == 0)); checks++;
        /* odd-even */
        l = odd_even(l);
        int oe[40], m = 0; for (int i = 0; i < en; i += 2) oe[m++] = out[i]; for (int i = 1; i < en; i += 2) oe[m++] = out[i];
        CHECK(to(l, got) == en && (en == 0 || memcmp(got, oe, (size_t)en * sizeof(int)) == 0)); checks++;
        destroy(l);
    }
    /* intersection: shared tail */
    Node *tail = from(base, 4), *x = mk(100, mk(101, tail)), *y = mk(200, tail);
    printf("intersection value: %d\n", intersection(x, y)->v);
    Node *z = mk(300, NULL); CHECK(intersection(x, z) == NULL);
    destroy(z);
    x->next->next = NULL; destroy(x); /* detach the shared tail before freeing x */
    destroy(y);
    printf("randomized checks=%ld\n", checks);
    return 0;
}
