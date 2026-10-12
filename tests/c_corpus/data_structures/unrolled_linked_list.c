/*
 * title: Unrolled linked list with split and merge
 * topic: data_structures
 * covers: unrolled linked list, block split, block merge, index lookup, cache-friendly lists, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xBB67AE8584CAA73BULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 16); }

#define B 8 /* block capacity */
typedef struct Block { int n; int a[B]; struct Block *next; } Block;
typedef struct { Block *head; size_t len, blocks; long splits, merges; } UList;

static Block *blk(void) { Block *b = calloc(1, sizeof *b); CHECK(b); return b; }

static Block *locate(UList *l, size_t *i) {
    Block *b = l->head;
    while (b && *i >= (size_t)b->n) { *i -= (size_t)b->n; b = b->next; }
    return b;
}
static void insert_at(UList *l, size_t i, int v) {
    CHECK(i <= l->len);
    if (!l->head) { l->head = blk(); l->blocks = 1; }
    size_t off = i;
    Block *b = locate(l, &off);
    if (!b) { /* append at end: use last block */
        b = l->head; while (b->next) b = b->next;
        off = (size_t)b->n;
    }
    if (b->n == B) {
        Block *nb = blk();
        int half = B / 2;
        memcpy(nb->a, b->a + half, (size_t)(B - half) * sizeof(int));
        nb->n = B - half; b->n = half;
        nb->next = b->next; b->next = nb; l->blocks++; l->splits++;
        if (off > (size_t)half) { off -= (size_t)half; b = nb; }
    }
    memmove(b->a + off + 1, b->a + off, ((size_t)b->n - off) * sizeof(int));
    b->a[off] = v; b->n++; l->len++;
}
static int erase_at(UList *l, size_t i) {
    CHECK(i < l->len);
    size_t off = i;
    Block *prev = NULL, *b = l->head;
    while (off >= (size_t)b->n) { off -= (size_t)b->n; prev = b; b = b->next; }
    int v = b->a[off];
    memmove(b->a + off, b->a + off + 1, ((size_t)b->n - off - 1) * sizeof(int));
    b->n--; l->len--;
    /* merge with the next block when both fit into one, drop empty blocks */
    if (b->n == 0) {
        if (prev) prev->next = b->next; else l->head = b->next;
        free(b); l->blocks--;
    } else if (b->next && b->n + b->next->n <= B * 3 / 4) {
        Block *nx = b->next;
        memcpy(b->a + b->n, nx->a, (size_t)nx->n * sizeof(int));
        b->n += nx->n; b->next = nx->next; free(nx); l->blocks--; l->merges++;
    }
    return v;
}
static int get(UList *l, size_t i) { CHECK(i < l->len); Block *b = locate(l, &i); return b->a[i]; }

#define MAXN 2000
int main(void) {
    UList l = {0};
    static int m[MAXN]; size_t mn = 0;
    long ins = 0, era = 0, gets = 0;
    size_t max_blocks = 0;
    for (int step = 0; step < 25000; step++) {
        int phase = (step / 2500) % 2;
        int pins = phase == 0 ? 70 : 35;
        if ((int)(rnd() % 100) < pins && mn < 1500) {
            size_t i = rnd() % (mn + 1); int v = (int)(rnd() % 100000);
            insert_at(&l, i, v);
            memmove(m + i + 1, m + i, (mn - i) * sizeof(int)); m[i] = v; mn++; ins++;
        } else if (mn && rnd() % 4) {
            size_t i = rnd() % mn;
            CHECK(erase_at(&l, i) == m[i]);
            memmove(m + i, m + i + 1, (mn - i - 1) * sizeof(int)); mn--; era++;
        } else if (mn) {
            size_t i = rnd() % mn; CHECK(get(&l, i) == m[i]); gets++;
        }
        CHECK(l.len == mn);
        if (l.blocks > max_blocks) max_blocks = l.blocks;
        if (step % 100 == 0) {
            size_t k = 0, nb = 0;
            for (Block *b = l.head; b; b = b->next, nb++) {
                CHECK(b->n > 0 && b->n <= B);
                for (int j = 0; j < b->n; j++) CHECK(b->a[j] == m[k++]);
            }
            CHECK(k == mn && nb == l.blocks);
        }
    }
    printf("insert=%ld erase=%ld get=%ld\n", ins, era, gets);
    printf("len=%zu blocks=%zu max_blocks=%zu splits=%ld merges=%ld\n", l.len, l.blocks, max_blocks, l.splits, l.merges);
    printf("fill factor x100: %zu\n", l.blocks ? l.len * 100 / (l.blocks * B) : 0);
    for (Block *b = l.head; b;) { Block *n = b->next; free(b); b = n; }
    return 0;
}
