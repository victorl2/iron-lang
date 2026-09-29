/*
 * title: Block-based deque with a block map
 * topic: data_structures
 * covers: chunked deque, block map, stable element addresses, index arithmetic, map recentering
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0xD807AA98A3030242ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 28); }

#define BS 8 /* elements per block */
typedef struct {
    int **map; size_t map_cap;
    size_t start;  /* absolute position of the first element: block = start / BS within map */
    size_t len;
    long block_allocs, block_frees, recenters;
} Deque;

static int *block_new(Deque *d) { int *b = malloc(BS * sizeof(int)); CHECK(b); d->block_allocs++; return b; }
static void dq_init(Deque *d) {
    d->map_cap = 4; d->map = calloc(d->map_cap, sizeof(int *)); CHECK(d->map);
    d->start = d->map_cap / 2 * BS; d->len = 0; d->block_allocs = d->block_frees = d->recenters = 0;
}
static int *slot(Deque *d, size_t abs) { return &d->map[abs / BS][abs % BS]; }
static void reserve_abs(Deque *d, size_t abs) {
    size_t b = abs / BS;
    CHECK(b < d->map_cap);
    if (!d->map[b]) d->map[b] = block_new(d);
}
static void recenter(Deque *d, size_t want_blocks) {
    /* new map twice as large; live blocks are moved so they sit in the middle */
    size_t first = d->len ? d->start / BS : 0, last = d->len ? (d->start + d->len - 1) / BS : 0;
    size_t used = d->len ? last - first + 1 : 0;
    size_t ncap = d->map_cap; while (ncap < (used + want_blocks) * 2 + 2) ncap *= 2;
    int **nm = calloc(ncap, sizeof(int *)); CHECK(nm);
    size_t nfirst = (ncap - used) / 2;
    for (size_t i = 0; i < used; i++) nm[nfirst + i] = d->map[first + i];
    /* free spare blocks (there should be none outside the live range) */
    for (size_t i = 0; i < d->map_cap; i++) if (d->map[i] && (i < first || i > last || !d->len)) { free(d->map[i]); d->block_frees++; }
    free(d->map); d->map = nm; d->map_cap = ncap;
    d->start = nfirst * BS + (d->len ? d->start % BS : BS / 2);
    d->recenters++;
}
static void push_back(Deque *d, int v) {
    size_t abs = d->start + d->len;
    if (abs / BS >= d->map_cap) { recenter(d, 1); abs = d->start + d->len; }
    reserve_abs(d, abs); *slot(d, abs) = v; d->len++;
}
static void push_front(Deque *d, int v) {
    if (d->start == 0) recenter(d, 1);
    d->start--; reserve_abs(d, d->start); *slot(d, d->start) = v; d->len++;
}
static int pop_front(Deque *d) {
    CHECK(d->len);
    int v = *slot(d, d->start); d->start++; d->len--;
    if (d->start % BS == 0) { size_t b = d->start / BS - 1; if (d->map[b]) { free(d->map[b]); d->map[b] = NULL; d->block_frees++; } }
    return v;
}
static int pop_back(Deque *d) {
    CHECK(d->len);
    size_t abs = d->start + d->len - 1;
    int v = *slot(d, abs); d->len--;
    if (abs % BS == 0 && d->map[abs / BS]) { free(d->map[abs / BS]); d->map[abs / BS] = NULL; d->block_frees++; }
    return v;
}
static int *at(Deque *d, size_t i) { CHECK(i < d->len); return slot(d, d->start + i); }
static void dq_free(Deque *d) {
    for (size_t i = 0; i < d->map_cap; i++) if (d->map[i]) { free(d->map[i]); d->block_frees++; }
    free(d->map);
}

#define MAXN 4000
int main(void) {
    Deque d; dq_init(&d);
    static int m[MAXN]; size_t mn = 0;
    long cnt[5] = {0}, addr_stable = 0;
    for (int step = 0; step < 45000; step++) {
        unsigned op = rnd() % 100;
        int v = (int)(rnd() % 1000000);
        int phase = (step / 5000) % 2;
        if (mn >= 3000) op = 99;
        if (phase == 1 && op < 40) op = 40 + op % 60;
        if (op < 25) { push_back(&d, v); m[mn++] = v; cnt[0]++; }
        else if (op < 50) { push_front(&d, v); memmove(m + 1, m, mn * sizeof(int)); m[0] = v; mn++; cnt[1]++; }
        else if (op < 75 && mn) { CHECK(pop_front(&d) == m[0]); memmove(m, m + 1, --mn * sizeof(int)); cnt[2]++; }
        else if (op < 95 && mn) { CHECK(pop_back(&d) == m[--mn]); cnt[3]++; }
        else if (mn) { size_t i = rnd() % mn; CHECK(*at(&d, i) == m[i]); *at(&d, i) = v; m[i] = v; cnt[4]++; }
        CHECK(d.len == mn);
        if (mn) { /* element addresses do not move when pushing at the ends */
            int *p = at(&d, mn / 2); int val = *p;
            if (op < 50) { push_back(&d, 1); pop_back(&d); }
            CHECK(at(&d, mn / 2) == p && *p == val); addr_stable++;
        }
        if (step % 500 == 0) for (size_t i = 0; i < mn; i++) CHECK(*at(&d, i) == m[i]);
    }
    printf("push_back=%ld push_front=%ld pop_front=%ld pop_back=%ld set=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
    printf("len=%zu map_cap=%zu recenters=%ld block_allocs=%ld addr_checks=%ld\n", d.len, d.map_cap, d.recenters, d.block_allocs, addr_stable);
    dq_free(&d);
    CHECK(d.block_allocs == d.block_frees);
    printf("blocks balanced: allocs == frees\n");
    return 0;
}
