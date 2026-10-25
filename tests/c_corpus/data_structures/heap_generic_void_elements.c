/*
 * title: Generic heap over untyped elements with comparator context
 * topic: data_structures
 * covers: generic container, void pointers, element size, comparator with context, memcpy element moves, replace-top, remove-at, heapsort
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x6E9E41Cull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef int (*Cmp)(const void *a, const void *b, void *ctx);

typedef struct {
    unsigned char *data;
    size_t esz;
    int n, cap;
    Cmp cmp;
    void *ctx;
    long compares;
} GHeap;

enum { MAXE = 64 };

static void gh_init(GHeap *h, size_t esz, Cmp cmp, void *ctx) {
    check(esz <= MAXE, "element size fits the scratch buffer");
    h->data = NULL;
    h->esz = esz;
    h->n = h->cap = 0;
    h->cmp = cmp;
    h->ctx = ctx;
    h->compares = 0;
}

static void *at(const GHeap *h, int i) { return h->data + (size_t)i * h->esz; }

static int lt(GHeap *h, const void *a, const void *b) {
    h->compares++;
    return h->cmp(a, b, h->ctx) < 0;
}

static void sift_up(GHeap *h, int i) {
    unsigned char tmp[MAXE];
    memcpy(tmp, at(h, i), h->esz);
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!lt(h, tmp, at(h, p)))
            break;
        memcpy(at(h, i), at(h, p), h->esz);
        i = p;
    }
    memcpy(at(h, i), tmp, h->esz);
}

static void sift_down(GHeap *h, int i) {
    unsigned char tmp[MAXE];
    memcpy(tmp, at(h, i), h->esz);
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n)
            break;
        if (c + 1 < h->n && lt(h, at(h, c + 1), at(h, c)))
            c++;
        if (!lt(h, at(h, c), tmp))
            break;
        memcpy(at(h, i), at(h, c), h->esz);
        i = c;
    }
    memcpy(at(h, i), tmp, h->esz);
}

static void gh_push(GHeap *h, const void *e) {
    if (h->n == h->cap) {
        h->cap = h->cap ? 2 * h->cap : 8;
        h->data = realloc(h->data, (size_t)h->cap * h->esz);
    }
    memcpy(at(h, h->n), e, h->esz);
    sift_up(h, h->n++);
}

static void gh_pop(GHeap *h, void *out) {
    memcpy(out, at(h, 0), h->esz);
    h->n--;
    if (h->n > 0) {
        memcpy(at(h, 0), at(h, h->n), h->esz);
        sift_down(h, 0);
    }
}

/* pop the top and push e in one sift */
static void gh_replace_top(GHeap *h, const void *e, void *out) {
    memcpy(out, at(h, 0), h->esz);
    memcpy(at(h, 0), e, h->esz);
    sift_down(h, 0);
}

static void gh_remove_at(GHeap *h, int i) {
    h->n--;
    if (i == h->n)
        return;
    memcpy(at(h, i), at(h, h->n), h->esz);
    sift_up(h, i);
    sift_down(h, i);
}

static int gh_valid(GHeap *h) {
    for (int i = 1; i < h->n; i++)
        if (h->cmp(at(h, i), at(h, (i - 1) / 2), h->ctx) < 0)
            return 0;
    return 1;
}

/* ---- element types ---- */
typedef struct {
    const char *name;
    int age;
    int id;
} Person; /* 16 bytes on LP64 */

typedef struct {
    int x, y;
    long tag;
    unsigned char pad[16];
} Point; /* 32 bytes */

typedef struct {
    int px, py;
    int calls;
} Pivot;

static int cmp_int(const void *a, const void *b, void *ctx) {
    (void)ctx;
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_int_desc(const void *a, const void *b, void *ctx) { return cmp_int(b, a, ctx); }

static int cmp_person(const void *a, const void *b, void *ctx) {
    (void)ctx;
    const Person *p = a, *q = b;
    if (p->age != q->age)
        return p->age < q->age ? -1 : 1;
    int c = strcmp(p->name, q->name);
    if (c)
        return c < 0 ? -1 : 1;
    return (p->id > q->id) - (p->id < q->id);
}

static int cmp_str(const void *a, const void *b, void *ctx) {
    (void)ctx;
    int c = strcmp(*(const char *const *)a, *(const char *const *)b);
    return (c > 0) - (c < 0);
}

static long dist2(const Point *p, const Pivot *v) {
    long dx = p->x - v->px, dy = p->y - v->py;
    return dx * dx + dy * dy;
}

static int cmp_dist(const void *a, const void *b, void *ctx) {
    Pivot *v = ctx;
    v->calls++;
    long da = dist2(a, v), db = dist2(b, v);
    if (da != db)
        return da < db ? -1 : 1;
    const Point *p = a, *q = b;
    return (p->tag > q->tag) - (p->tag < q->tag);
}

int main(void) {
    /* ints, min-heap and max-heap, against qsort */
    for (int dir = 0; dir < 2; dir++) {
        GHeap h;
        gh_init(&h, sizeof(int), dir ? cmp_int_desc : cmp_int, NULL);
        int ref[500];
        for (int i = 0; i < 500; i++) {
            ref[i] = (int)(rng() % 1000);
            gh_push(&h, &ref[i]);
        }
        check(gh_valid(&h), "int heap valid");
        long sum = 0;
        int v, prev = dir ? 100000 : -1, first = 0;
        for (int i = 0; i < 500; i++) {
            gh_pop(&h, &v);
            check(dir ? v <= prev : v >= prev, "ints pop in order");
            prev = v;
            if (i == 0)
                first = v;
            sum += (long)v * (i % 7);
        }
        printf("ints %s: first=%d weighted_sum=%ld compares=%ld\n", dir ? "max" : "min", first, sum, h.compares);
        free(h.data);
    }

    /* records with strings */
    static const char *names[] = {"ada", "bob", "cyd", "dee", "eli", "fay", "gus", "hal"};
    GHeap ph;
    gh_init(&ph, sizeof(Person), cmp_person, NULL);
    Person people[200];
    for (int i = 0; i < 200; i++) {
        people[i] = (Person){names[rng() % 8], 18 + (int)(rng() % 10), i};
        gh_push(&ph, &people[i]);
    }
    Person p, prevp = {"", 0, -1};
    for (int i = 0; i < 200; i++) {
        gh_pop(&ph, &p);
        check(i == 0 || cmp_person(&prevp, &p, NULL) < 0, "people strictly increasing under the total order");
        if (i < 3)
            printf("person %d: %s %d #%d\n", i, p.name, p.age, p.id);
        prevp = p;
    }
    free(ph.data);

    /* string pointers, heapsort and replace-top as a bounded "largest 5 words" filter */
    static const char *words[] = {"pear", "fig", "plum", "apple", "kiwi", "lime", "date", "grape", "melon", "peach", "cherry", "lemon"};
    GHeap sh;
    gh_init(&sh, sizeof(char *), cmp_str, NULL);
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (sh.n < 5)
            gh_push(&sh, &words[i]);
        else if (strcmp(words[i], *(const char **)at(&sh, 0)) > 0) {
            const char *out;
            gh_replace_top(&sh, &words[i], &out);
        }
    }
    printf("largest five words:");
    const char *w;
    while (sh.n > 0) {
        gh_pop(&sh, &w);
        printf(" %s", w);
    }
    printf("\n");
    free(sh.data);

    /* comparator with context: nearest points to a pivot, with remove-at */
    Pivot pv = {50, 50, 0};
    GHeap qh;
    gh_init(&qh, sizeof(Point), cmp_dist, &pv);
    Point pts[300];
    for (int i = 0; i < 300; i++) {
        memset(&pts[i], 0, sizeof(Point));
        pts[i].x = (int)(rng() % 101);
        pts[i].y = (int)(rng() % 101);
        pts[i].tag = i;
        gh_push(&qh, &pts[i]);
    }
    for (int k = 0; k < 40; k++) { /* remove some random elements by position */
        int at_i = (int)(rng() % (unsigned)qh.n);
        gh_remove_at(&qh, at_i);
        check(gh_valid(&qh), "valid after remove-at");
    }
    Point pt;
    long dsum = 0;
    long last = -1;
    int cnt = 0;
    while (qh.n > 0) {
        gh_pop(&qh, &pt);
        long d = dist2(&pt, &pv);
        check(d >= last, "points come out nearest first");
        last = d;
        if (cnt < 5)
            dsum += d;
        cnt++;
    }
    printf("points left=%d nearest5_dist2_sum=%ld farthest_dist2=%ld comparator_calls=%d\n", cnt, dsum, last, pv.calls);
    free(qh.data);
    return 0;
}
