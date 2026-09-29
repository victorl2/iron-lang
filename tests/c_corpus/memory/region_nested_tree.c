/*
 * title: Nested regions over a shared page pool
 * topic: memory
 * covers: region allocator, parent/child regions, bulk free of subtree, page free list, page accounting invariant
 * deps: libc
 */
#define SEED 0x5EED0F7EE5ULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}
static void pat_fill(void *vp, size_t n, unsigned tag) {
    unsigned char *p = vp;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u);
}
static int pat_ok(const void *vp, size_t n, unsigned tag) {
    const unsigned char *p = vp;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u)) return 0;
    return 1;
}

#define PAGE 256
#define NPAGES 160
#define MAXREG 24
#define NIL (-1)

static _Alignas(16) unsigned char pages[NPAGES * PAGE];
static int page_next[NPAGES];   /* link: free list or region's page chain */
static int free_head, free_count;

typedef struct {
    int alive, parent, first_child, next_sibling;
    int page_head, npages;
    size_t used_in_page;
    size_t bytes;
} Region;

static Region reg[MAXREG];

static void pages_init(void) {
    for (int i = 0; i < NPAGES; i++) page_next[i] = i + 1 < NPAGES ? i + 1 : NIL;
    free_head = 0; free_count = NPAGES;
}

static int region_new(int parent) {
    for (int i = 0; i < MAXREG; i++) {
        if (!reg[i].alive) {
            Region *r = &reg[i];
            memset(r, 0, sizeof *r);
            r->alive = 1; r->parent = parent; r->first_child = NIL; r->next_sibling = NIL;
            r->page_head = NIL; r->used_in_page = PAGE;
            if (parent != NIL) { r->next_sibling = reg[parent].first_child; reg[parent].first_child = i; }
            return i;
        }
    }
    return NIL;
}

static void *region_alloc(int id, size_t n) {
    Region *r = &reg[id];
    CHECK(r->alive && n <= PAGE);
    n = (n + 7) & ~(size_t)7;
    if (r->used_in_page + n > PAGE) {
        if (free_head == NIL) return NULL;
        int pg = free_head;
        free_head = page_next[pg]; free_count--;
        page_next[pg] = r->page_head; r->page_head = pg; r->npages++;
        r->used_in_page = 0;
    }
    void *p = pages + (size_t)r->page_head * PAGE + r->used_in_page;
    r->used_in_page += n;
    r->bytes += n;
    return p;
}

static int region_destroy(int id) { /* returns pages released, destroys the whole subtree */
    Region *r = &reg[id];
    int released = 0;
    int c = r->first_child;
    while (c != NIL) { int nx = reg[c].next_sibling; released += region_destroy(c); c = nx; }
    for (int pg = r->page_head; pg != NIL;) {
        int nx = page_next[pg];
        memset(pages + (size_t)pg * PAGE, 0xEE, PAGE);
        page_next[pg] = free_head; free_head = pg; free_count++; released++;
        pg = nx;
    }
    if (r->parent != NIL) {
        Region *p = &reg[r->parent];
        if (p->first_child == id) p->first_child = r->next_sibling;
        else for (int s = p->first_child; s != NIL; s = reg[s].next_sibling)
            if (reg[s].next_sibling == id) { reg[s].next_sibling = r->next_sibling; break; }
    }
    r->alive = 0;
    return released;
}

static void check_pages(void) {
    int held = 0;
    for (int i = 0; i < MAXREG; i++) if (reg[i].alive) {
        int cnt = 0;
        for (int pg = reg[i].page_head; pg != NIL; pg = page_next[pg]) cnt++;
        CHECK(cnt == reg[i].npages);
        held += cnt;
    }
    int f = 0;
    for (int pg = free_head; pg != NIL; pg = page_next[pg]) f++;
    CHECK(f == free_count);
    CHECK(held + free_count == NPAGES);
}

typedef struct { int region; unsigned char *p; size_t n; unsigned tag; } Rec;

int main(void) {
    pages_init();
    static Rec live[4000];
    int nlive = 0, fails = 0, created = 0, destroyed = 0, released_total = 0, max_depth = 0;
    unsigned tag = 1;
    int root = region_new(NIL);
    created++;

    for (int step = 0; step < 3000; step++) {
        unsigned op = rnd() % 100;
        int alive_ids[MAXREG], na = 0;
        for (int i = 0; i < MAXREG; i++) if (reg[i].alive) alive_ids[na++] = i;
        int target = alive_ids[rnd() % (unsigned)na];
        if (op < 66) {
            size_t n = 1 + rnd() % 90;
            unsigned char *p = region_alloc(target, n);
            if (!p) { fails++; continue; }
            pat_fill(p, n, tag);
            live[nlive].region = target; live[nlive].p = p; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
            CHECK(nlive < 4000);
        } else if (op < 78) {
            int r = region_new(target);
            if (r != NIL) {
                created++;
                int d = 0;
                for (int x = r; reg[x].parent != NIL; x = reg[x].parent) d++;
                if (d > max_depth) max_depth = d;
            }
        } else if (op < 97 && target != root) {
            int rel = region_destroy(target);
            released_total += rel;
            destroyed++;
            int w = 0;
            for (int i = 0; i < nlive; i++) if (!(reg[live[i].region].alive) ) continue; else live[w++] = live[i];
            nlive = w;
        } else if (op >= 97) {
            /* destroy everything under root, keep root */
            for (int i = 0; i < MAXREG; i++)
                if (reg[i].alive && reg[i].parent == root) { released_total += region_destroy(i); destroyed++; }
            int w = 0;
            for (int i = 0; i < nlive; i++) if (reg[live[i].region].alive) live[w++] = live[i];
            nlive = w;
        }
        if (step % 40 == 0) {
            check_pages();
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(live[i].p, live[i].n, live[i].tag));
        }
    }
    check_pages();
    for (int i = 0; i < nlive; i++) CHECK(pat_ok(live[i].p, live[i].n, live[i].tag));
    int alive = 0;
    for (int i = 0; i < MAXREG; i++) alive += reg[i].alive;
    printf("regions created=%d destroyed=%d alive=%d max depth=%d\n", created, destroyed, alive, max_depth);
    printf("pages released by destroys=%d free now=%d of %d\n", released_total, free_count, NPAGES);
    printf("failed allocs=%d live records=%d\n", fails, nlive);
    released_total = region_destroy(root);
    CHECK(free_count == NPAGES);
    printf("root destroyed, %d pages returned, all %d free\n", released_total, free_count);
    return 0;
}
