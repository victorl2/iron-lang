/*
 * title: Doubly linked list splice, unique and partition operations
 * topic: data_structures
 * covers: list splice, unique, stable partition, reverse, remove_if, node relinking without allocation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x71374491B5C0FBCFULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 18); }

typedef struct N { int v; struct N *prev, *next; } N;
typedef struct { N s; size_t len; } L; /* sentinel-based */

static void l_init(L *l) { l->s.prev = l->s.next = &l->s; l->len = 0; }
static void link_before(L *l, N *pos, N *n) { n->next = pos; n->prev = pos->prev; pos->prev->next = n; pos->prev = n; l->len++; }
static N *unlink_node(L *l, N *n) { n->prev->next = n->next; n->next->prev = n->prev; l->len--; return n; }
static void push_back(L *l, int v) { N *n = malloc(sizeof *n); CHECK(n); n->v = v; link_before(l, &l->s, n); }
static void clear(L *l) { while (l->len) free(unlink_node(l, l->s.next)); }
/* move [first, last) from src to before pos in dst (nodes are relinked, not copied) */
static void splice(L *dst, N *pos, L *src, N *first, N *last) {
    while (first != last) { N *nx = first->next; link_before(dst, pos, unlink_node(src, first)); first = nx; }
}
static void reverse(L *l) {
    N *p = &l->s;
    do { N *t = p->next; p->next = p->prev; p->prev = t; p = t; } while (p != &l->s);
}
static size_t unique(L *l) {
    size_t n = 0;
    for (N *p = l->s.next; p != &l->s && p->next != &l->s;) {
        if (p->next->v == p->v) { free(unlink_node(l, p->next)); n++; } else p = p->next;
    }
    return n;
}
/* stable partition: nodes with even values first. Returns the boundary node. */
static N *partition_even(L *l) {
    L odd; l_init(&odd);
    for (N *p = l->s.next, *nx; p != &l->s; p = nx) { nx = p->next; if (p->v % 2) link_before(&odd, &odd.s, unlink_node(l, p)); }
    N *boundary = &l->s;
    splice(l, &l->s, &odd, odd.s.next, &odd.s);
    boundary = l->s.next;
    return boundary;
}
static void merge_sorted(L *a, L *b) { /* both sorted ascending, stable */
    N *p = a->s.next;
    while (b->len) {
        N *q = b->s.next;
        while (p != &a->s && p->v <= q->v) p = p->next;
        link_before(a, p, unlink_node(b, q));
    }
}

#define MAXN 200
static int to_arr(const L *l, int *a) { int n = 0; for (N *p = l->s.next; p != &l->s; p = p->next) { CHECK(p->next->prev == p); a[n++] = p->v; } CHECK((size_t)n == l->len); return n; }
static void fill(L *l, int *m, int *mn, int n, int range) { for (int i = 0; i < n; i++) { int v = (int)(rnd() % (unsigned)range); push_back(l, v); m[(*mn)++] = v; } }

int main(void) {
    long checks = 0, moved = 0, uniq_total = 0;
    for (int round = 0; round < 300; round++) {
        L a, b; l_init(&a); l_init(&b);
        int ma[MAXN], mb[MAXN], na = 0, nb = 0, out[2 * MAXN];
        fill(&a, ma, &na, (int)(rnd() % 30), 8);
        fill(&b, mb, &nb, (int)(rnd() % 30), 8);
        /* splice a random range of b into a random position of a */
        int i0 = na ? (int)(rnd() % (unsigned)(na + 1)) : 0;
        int j0 = nb ? (int)(rnd() % (unsigned)(nb + 1)) : 0, j1 = j0 + (nb - j0 ? (int)(rnd() % (unsigned)(nb - j0 + 1)) : 0);
        N *pos = a.s.next; for (int i = 0; i < i0; i++) pos = pos->next;
        N *f = b.s.next; for (int i = 0; i < j0; i++) f = f->next;
        N *e = f; for (int i = j0; i < j1; i++) e = e->next;
        splice(&a, pos, &b, f, e);
        moved += j1 - j0;
        int m2[2 * MAXN], n2 = 0;
        for (int i = 0; i < i0; i++) m2[n2++] = ma[i];
        for (int i = j0; i < j1; i++) m2[n2++] = mb[i];
        for (int i = i0; i < na; i++) m2[n2++] = ma[i];
        int rest[MAXN], nr = 0; for (int i = 0; i < j0; i++) rest[nr++] = mb[i]; for (int i = j1; i < nb; i++) rest[nr++] = mb[i];
        CHECK(to_arr(&a, out) == n2 && memcmp(out, m2, (size_t)n2 * sizeof(int)) == 0);
        CHECK(to_arr(&b, out) == nr && memcmp(out, rest, (size_t)nr * sizeof(int)) == 0);
        checks += 2;
        /* reverse */
        reverse(&a);
        for (int i = 0; i < n2 / 2; i++) { int t = m2[i]; m2[i] = m2[n2 - 1 - i]; m2[n2 - 1 - i] = t; }
        CHECK(to_arr(&a, out) == n2 && memcmp(out, m2, (size_t)n2 * sizeof(int)) == 0); checks++;
        /* partition even-first, stable */
        partition_even(&a);
        int pm[2 * MAXN], k = 0;
        for (int i = 0; i < n2; i++) if (m2[i] % 2 == 0) pm[k++] = m2[i];
        for (int i = 0; i < n2; i++) if (m2[i] % 2) pm[k++] = m2[i];
        CHECK(to_arr(&a, out) == n2 && memcmp(out, pm, (size_t)n2 * sizeof(int)) == 0); checks++;
        /* unique on adjacent duplicates */
        size_t u = unique(&a);
        int um[2 * MAXN], un = 0;
        for (int i = 0; i < n2; i++) if (!un || um[un - 1] != pm[i]) um[un++] = pm[i];
        CHECK(u == (size_t)(n2 - un));
        CHECK(to_arr(&a, out) == un && memcmp(out, um, (size_t)un * sizeof(int)) == 0); checks++;
        uniq_total += (long)u;
        clear(&a); clear(&b);
        /* merge of sorted lists */
        int sa[MAXN], sb[MAXN], sn = 0, tn = 0;
        int va = 0; for (int i = 0, n = (int)(rnd() % 20); i < n; i++) { va += (int)(rnd() % 4); push_back(&a, va); sa[sn++] = va; }
        int vb = 0; for (int i = 0, n = (int)(rnd() % 20); i < n; i++) { vb += (int)(rnd() % 4); push_back(&b, vb); sb[tn++] = vb; }
        merge_sorted(&a, &b);
        int mm[2 * MAXN], mk = 0, x = 0, y = 0;
        while (x < sn || y < tn) { if (y >= tn || (x < sn && sa[x] <= sb[y])) mm[mk++] = sa[x++]; else mm[mk++] = sb[y++]; }
        CHECK(to_arr(&a, out) == mk && memcmp(out, mm, (size_t)mk * sizeof(int)) == 0 && b.len == 0); checks++;
        clear(&a);
    }
    printf("rounds=300 checks=%ld nodes_spliced=%ld duplicates_removed=%ld\n", checks, moved, uniq_total);
    L l; l_init(&l);
    static const int demo[] = { 5, 2, 2, 7, 4, 4, 4, 9, 6, 1, 1 };
    for (size_t i = 0; i < sizeof demo / sizeof demo[0]; i++) push_back(&l, demo[i]);
    partition_even(&l); unique(&l); reverse(&l);
    int out[32]; int n = to_arr(&l, out);
    printf("demo:"); for (int i = 0; i < n; i++) printf(" %d", out[i]);
    printf("\n");
    clear(&l);
    return 0;
}
