/*
 * title: Generic circular sentinel list with stable iterators and range operations
 * topic: data_structures
 * covers: type-erased list, sentinel node, iterators that survive mutation, splice, rotate, stable merge sort, unique, reverse iteration
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 1029384u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Node { struct Node *prev, *next; } Node;
#define HDR ((sizeof(Node) + 15u) & ~(size_t)15u)
typedef int (*CmpFn)(const void *, const void *);
typedef struct { Node s; size_t esz, len; } List;
typedef struct { Node *n; } Iter;

static void *deref(Iter it) { return (unsigned char *)it.n + HDR; }
static void l_init(List *l, size_t esz) { l->s.prev = l->s.next = &l->s; l->esz = esz; l->len = 0; }
static Iter it_begin(List *l) { Iter i = { l->s.next }; return i; }
static Iter it_end(List *l) { Iter i = { &l->s }; return i; }
static Iter it_next(Iter i) { i.n = i.n->next; return i; }
static Iter it_prev(Iter i) { i.n = i.n->prev; return i; }
static int it_eq(Iter a, Iter b) { return a.n == b.n; }
static Iter it_advance(Iter i, long k) {
    while (k > 0) { i = it_next(i); k--; }
    while (k < 0) { i = it_prev(i); k++; }
    return i;
}
static long it_distance(Iter a, Iter b) { long d = 0; while (!it_eq(a, b)) { a = it_next(a); d++; } return d; }

static Iter l_insert(List *l, Iter pos, const void *elem) {
    Node *n = malloc(HDR + l->esz);
    CHECK(n);
    memcpy((unsigned char *)n + HDR, elem, l->esz);
    n->next = pos.n; n->prev = pos.n->prev;
    pos.n->prev->next = n;
    pos.n->prev = n;
    l->len++;
    Iter r = { n };
    return r;
}
static Iter l_erase(List *l, Iter pos) {
    CHECK(pos.n != &l->s);
    Iter nx = { pos.n->next };
    pos.n->prev->next = pos.n->next;
    pos.n->next->prev = pos.n->prev;
    free(pos.n);
    l->len--;
    return nx;
}
static Iter l_erase_range(List *l, Iter a, Iter b) {
    while (!it_eq(a, b)) a = l_erase(l, a);
    return b;
}
/* move [first, last) out of src and in front of pos in dst: no allocation, iterators stay valid */
static void l_splice(List *dst, Iter pos, List *src, Iter first, Iter last) {
    if (it_eq(first, last)) return;
    long n = it_distance(first, last);
    Node *f = first.n, *lastn = last.n->prev;
    f->prev->next = last.n;
    last.n->prev = f->prev;
    f->prev = pos.n->prev;
    pos.n->prev->next = f;
    lastn->next = pos.n;
    pos.n->prev = lastn;
    src->len -= (size_t)n;
    dst->len += (size_t)n;
}
static void l_reverse(List *l) {
    Node *n = &l->s;
    do { Node *t = n->next; n->next = n->prev; n->prev = t; n = t; } while (n != &l->s);
}
static void l_rotate_left(List *l, size_t k) { /* first k elements move to the back */
    if (l->len == 0) return;
    k %= l->len;
    Iter mid = it_advance(it_begin(l), (long)k);
    l_splice(l, it_end(l), l, it_begin(l), mid);
}
static void l_clear(List *l) { l_erase_range(l, it_begin(l), it_end(l)); }
/* stable merge sort by relinking */
static void merge_into(List *out, List *a, List *b, CmpFn cmp) {
    while (a->len && b->len) {
        List *src = cmp(deref(it_begin(b)), deref(it_begin(a))) < 0 ? b : a;
        Iter f = it_begin(src);
        l_splice(out, it_end(out), src, f, it_next(f));
    }
    l_splice(out, it_end(out), a, it_begin(a), it_end(a));
    l_splice(out, it_end(out), b, it_begin(b), it_end(b));
}
static void l_sort(List *l, CmpFn cmp) {
    if (l->len < 2) return;
    List a, b;
    l_init(&a, l->esz); l_init(&b, l->esz);
    size_t half = l->len / 2;
    l_splice(&a, it_end(&a), l, it_begin(l), it_advance(it_begin(l), (long)half));
    l_splice(&b, it_end(&b), l, it_begin(l), it_end(l));
    l_sort(&a, cmp);
    l_sort(&b, cmp);
    merge_into(l, &a, &b, cmp);
}
static size_t l_unique(List *l, CmpFn cmp) {
    size_t removed = 0;
    Iter i = it_begin(l);
    while (!it_eq(i, it_end(l))) {
        Iter nx = it_next(i);
        if (!it_eq(nx, it_end(l)) && cmp(deref(i), deref(nx)) == 0) { l_erase(l, nx); removed++; }
        else i = nx;
    }
    return removed;
}
static size_t l_remove_if(List *l, int (*pred)(const void *, void *), void *ctx) {
    size_t removed = 0;
    Iter i = it_begin(l);
    while (!it_eq(i, it_end(l))) {
        if (pred(deref(i), ctx)) { i = l_erase(l, i); removed++; } else i = it_next(i);
    }
    return removed;
}

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
typedef struct { int prio, seq; char tag[6]; } Job;
static int cmp_job(const void *a, const void *b) { int x = ((const Job *)a)->prio, y = ((const Job *)b)->prio; return (x > y) - (x < y); }
static int is_multiple(const void *e, void *ctx) { return *(const int *)e % *(int *)ctx == 0; }

static void check_links(List *l) {
    size_t n = 0;
    for (Iter i = it_begin(l); !it_eq(i, it_end(l)); i = it_next(i)) {
        CHECK(i.n->next->prev == i.n && i.n->prev->next == i.n);
        n++;
        CHECK(n <= l->len);
    }
    CHECK(n == l->len);
}

int main(void) {
    /* int list against an array model, holding iterators across mutations */
    List l;
    l_init(&l, sizeof(int));
    int model[400], mn = 0;
    long splices = 0;
    for (int step = 0; step < 3000; step++) {
        unsigned op = rnd() % 16;
        int v = (int)(rnd() % 50);
        long pos = mn ? (long)(rnd() % (unsigned)(mn + 1)) : 0;
        Iter it = it_advance(it_begin(&l), pos);
        if (op < 7 && mn < 400) {
            Iter r = l_insert(&l, it, &v);
            CHECK(*(int *)deref(r) == v && it_distance(it_begin(&l), r) == pos);
            memmove(&model[pos + 1], &model[pos], (size_t)(mn - pos) * sizeof(int));
            model[pos] = v; mn++;
        } else if (op < 9 && pos < mn) {
            Iter keep = it_next(it);
            Iter r = l_erase(&l, it);
            CHECK(it_eq(r, keep)); /* other iterators stay valid across an erase */
            memmove(&model[pos], &model[pos + 1], (size_t)(mn - pos - 1) * sizeof(int));
            mn--;
        } else if (op < 10 && mn > 1) {
            long k = (long)(rnd() % (unsigned)mn);
            l_rotate_left(&l, (size_t)k);
            int tmp[400];
            for (int i = 0; i < mn; i++) tmp[i] = model[(i + k) % mn];
            memcpy(model, tmp, (size_t)mn * sizeof(int));
        } else if (op < 11) {
            l_reverse(&l);
            for (int a = 0, b = mn - 1; a < b; a++, b--) { int t = model[a]; model[a] = model[b]; model[b] = t; }
        } else if (op < 12 && mn > 2) {
            /* splice a middle range to the front through a second list and back to a random place */
            long a = (long)(rnd() % (unsigned)mn), b = a + (long)(rnd() % (unsigned)(mn - a + 1));
            List tmp;
            l_init(&tmp, sizeof(int));
            Iter fa = it_advance(it_begin(&l), a), fb = it_advance(fa, b - a);
            l_splice(&tmp, it_end(&tmp), &l, fa, fb);
            CHECK(tmp.len == (size_t)(b - a) && l.len == (size_t)(mn - (b - a)));
            long dpos = (long)(rnd() % (unsigned)(l.len + 1));
            l_splice(&l, it_advance(it_begin(&l), dpos), &tmp, it_begin(&tmp), it_end(&tmp));
            int chunk[400], rest[400], rn = 0, cn = 0;
            for (int i = 0; i < mn; i++) { if (i >= a && i < b) chunk[cn++] = model[i]; else rest[rn++] = model[i]; }
            int out = 0;
            for (int i = 0; i < dpos; i++) model[out++] = rest[i];
            for (int i = 0; i < cn; i++) model[out++] = chunk[i];
            for (int i = (int)dpos; i < rn; i++) model[out++] = rest[i];
            splices++;
        } else if (op < 13) {
            if (rnd() % 10 != 0) continue;
            int m = 2 + (int)(rnd() % 4);
            size_t rem = l_remove_if(&l, is_multiple, &m);
            int k = 0;
            for (int i = 0; i < mn; i++) if (model[i] % m != 0) model[k++] = model[i];
            CHECK(rem == (size_t)(mn - k));
            mn = k;
        } else if (op < 14 && mn > 1) {
            /* erase a short random range */
            long a = (long)(rnd() % (unsigned)mn), b = a + (long)(rnd() % (unsigned)((mn - a < 6 ? mn - a : 5) + 1));
            l_erase_range(&l, it_advance(it_begin(&l), a), it_advance(it_begin(&l), b));
            memmove(&model[a], &model[b], (size_t)(mn - b) * sizeof(int));
            mn -= (int)(b - a);
        }
        CHECK(l.len == (size_t)mn);
        if (step % 100 == 0) {
            check_links(&l);
            int i = 0;
            for (Iter x = it_begin(&l); !it_eq(x, it_end(&l)); x = it_next(x)) CHECK(*(int *)deref(x) == model[i++]);
            for (Iter x = it_end(&l); i > 0;) { x = it_prev(x); CHECK(*(int *)deref(x) == model[--i]); }
        }
    }
    l_sort(&l, cmp_int);
    for (int a = 0; a < mn; a++) for (int b = a + 1; b < mn; b++) if (model[b] < model[a]) { int t = model[a]; model[a] = model[b]; model[b] = t; }
    int i = 0;
    for (Iter x = it_begin(&l); !it_eq(x, it_end(&l)); x = it_next(x)) CHECK(*(int *)deref(x) == model[i++]);
    size_t dups = l_unique(&l, cmp_int);
    int k = 0;
    for (int a = 0; a < mn; a++) if (k == 0 || model[k - 1] != model[a]) model[k++] = model[a];
    CHECK(dups == (size_t)(mn - k) && l.len == (size_t)k);
    printf("int list: %zu distinct sorted values (removed %zu duplicates), splices=%ld\n", l.len, dups, splices);
    printf("  first=%d last=%d distance(begin+3,end)=%ld\n", *(int *)deref(it_begin(&l)), *(int *)deref(it_prev(it_end(&l))), it_distance(it_advance(it_begin(&l), 3), it_end(&l)));
    l_clear(&l);
    CHECK(l.len == 0 && l.s.next == &l.s);

    /* struct elements: the sort must be stable */
    List jl;
    l_init(&jl, sizeof(Job));
    for (int j = 0; j < 200; j++) {
        Job job;
        memset(&job, 0, sizeof job);
        job.prio = (int)(rnd() % 8);
        job.seq = j;
        snprintf(job.tag, sizeof job.tag, "j%d", j % 100);
        l_insert(&jl, it_end(&jl), &job);
    }
    l_sort(&jl, cmp_job);
    int stable = 1, prev_p = -1, prev_s = -1;
    int hist[8] = { 0 };
    for (Iter x = it_begin(&jl); !it_eq(x, it_end(&jl)); x = it_next(x)) {
        Job *j = deref(x);
        if (j->prio == prev_p && j->seq < prev_s) stable = 0;
        CHECK(j->prio >= prev_p);
        prev_p = j->prio; prev_s = j->seq;
        hist[j->prio]++;
    }
    CHECK(stable);
    printf("jobs: stable sort of %zu records, priority histogram", jl.len);
    for (int p = 0; p < 8; p++) printf(" %d", hist[p]);
    printf("\n");
    l_clear(&jl);
    return 0;
}
