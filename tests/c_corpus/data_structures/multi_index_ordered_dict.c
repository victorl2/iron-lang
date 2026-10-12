/*
 * title: Ordered dictionary with hash, sorted and group indexes kept consistent
 * topic: data_structures
 * covers: multi-index container, insertion-order links, unique hash index, sorted secondary index, intrusive group lists, invariant audit
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 24680u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define MAXREC 300
#define NGROUP 6
#define NBUCKET 64

typedef struct {
    int alive;
    char name[12];
    int score, group;
    int order_prev, order_next;      /* insertion order (doubly linked, -1 terminated) */
    int hash_next;                   /* chain in the name index */
    int grp_prev, grp_next;          /* intrusive list per group */
} Rec;

typedef struct {
    Rec r[MAXREC];
    int nslots;
    int free_head;                   /* recycled slots chained through hash_next */
    int order_head, order_tail;
    int bucket[NBUCKET];
    int grp_head[NGROUP];
    int by_score[MAXREC], nscore;    /* sorted by (score, slot) */
    int count;
} Dict;

static unsigned hname(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % NBUCKET;
}
static void d_init(Dict *d) {
    memset(d, 0, sizeof *d);
    d->free_head = d->order_head = d->order_tail = -1;
    for (int i = 0; i < NBUCKET; i++) d->bucket[i] = -1;
    for (int i = 0; i < NGROUP; i++) d->grp_head[i] = -1;
}
static int d_find(const Dict *d, const char *name) {
    for (int i = d->bucket[hname(name)]; i >= 0; i = d->r[i].hash_next)
        if (strcmp(d->r[i].name, name) == 0) return i;
    return -1;
}
static int score_less(const Dict *d, int a, int b) {
    if (d->r[a].score != d->r[b].score) return d->r[a].score < d->r[b].score;
    return a < b;
}
static void score_insert(Dict *d, int slot) {
    int lo = 0, hi = d->nscore;
    while (lo < hi) { int m = (lo + hi) / 2; if (score_less(d, d->by_score[m], slot)) lo = m + 1; else hi = m; }
    memmove(&d->by_score[lo + 1], &d->by_score[lo], (size_t)(d->nscore - lo) * sizeof(int));
    d->by_score[lo] = slot;
    d->nscore++;
}
static void score_remove(Dict *d, int slot) {
    int lo = 0, hi = d->nscore;
    while (lo < hi) { int m = (lo + hi) / 2; if (score_less(d, d->by_score[m], slot)) lo = m + 1; else hi = m; }
    CHECK(lo < d->nscore && d->by_score[lo] == slot);
    memmove(&d->by_score[lo], &d->by_score[lo + 1], (size_t)(d->nscore - lo - 1) * sizeof(int));
    d->nscore--;
}
static void grp_link(Dict *d, int slot) {
    Rec *r = &d->r[slot];
    r->grp_prev = -1;
    r->grp_next = d->grp_head[r->group];
    if (r->grp_next >= 0) d->r[r->grp_next].grp_prev = slot;
    d->grp_head[r->group] = slot;
}
static void grp_unlink(Dict *d, int slot) {
    Rec *r = &d->r[slot];
    if (r->grp_prev >= 0) d->r[r->grp_prev].grp_next = r->grp_next; else d->grp_head[r->group] = r->grp_next;
    if (r->grp_next >= 0) d->r[r->grp_next].grp_prev = r->grp_prev;
}
static void order_append(Dict *d, int slot) {
    Rec *r = &d->r[slot];
    r->order_prev = d->order_tail; r->order_next = -1;
    if (d->order_tail >= 0) d->r[d->order_tail].order_next = slot; else d->order_head = slot;
    d->order_tail = slot;
}
static void order_unlink(Dict *d, int slot) {
    Rec *r = &d->r[slot];
    if (r->order_prev >= 0) d->r[r->order_prev].order_next = r->order_next; else d->order_head = r->order_next;
    if (r->order_next >= 0) d->r[r->order_next].order_prev = r->order_prev; else d->order_tail = r->order_prev;
}
static void name_link(Dict *d, int slot) {
    unsigned b = hname(d->r[slot].name);
    d->r[slot].hash_next = d->bucket[b];
    d->bucket[b] = slot;
}
static void name_unlink(Dict *d, int slot) {
    unsigned b = hname(d->r[slot].name);
    int *pp = &d->bucket[b];
    while (*pp != slot) { CHECK(*pp >= 0); pp = &d->r[*pp].hash_next; }
    *pp = d->r[slot].hash_next;
}

/* returns slot, or -1 if the name already exists (unique constraint) or the table is full */
static int d_insert(Dict *d, const char *name, int score, int group) {
    if (d_find(d, name) >= 0) return -1;
    int slot;
    if (d->free_head >= 0) { slot = d->free_head; d->free_head = d->r[slot].hash_next; }
    else if (d->nslots < MAXREC) slot = d->nslots++;
    else return -1;
    Rec *r = &d->r[slot];
    r->alive = 1;
    snprintf(r->name, sizeof r->name, "%s", name);
    r->score = score; r->group = group;
    name_link(d, slot); grp_link(d, slot); order_append(d, slot); score_insert(d, slot);
    d->count++;
    return slot;
}
static int d_erase(Dict *d, const char *name) {
    int slot = d_find(d, name);
    if (slot < 0) return 0;
    name_unlink(d, slot); grp_unlink(d, slot); order_unlink(d, slot); score_remove(d, slot);
    d->r[slot].alive = 0;
    d->r[slot].hash_next = d->free_head;
    d->free_head = slot;
    d->count--;
    return 1;
}
static int d_set_score(Dict *d, const char *name, int score) {
    int slot = d_find(d, name);
    if (slot < 0) return 0;
    score_remove(d, slot);
    d->r[slot].score = score;
    score_insert(d, slot);
    return 1;
}
static int d_set_group(Dict *d, const char *name, int group) {
    int slot = d_find(d, name);
    if (slot < 0) return 0;
    grp_unlink(d, slot);
    d->r[slot].group = group;
    grp_link(d, slot);
    return 1;
}
static int d_rename(Dict *d, const char *from, const char *to) {
    int slot = d_find(d, from);
    if (slot < 0 || d_find(d, to) >= 0) return 0;
    name_unlink(d, slot);
    snprintf(d->r[slot].name, sizeof d->r[slot].name, "%s", to);
    name_link(d, slot);
    return 1;
}
static int d_move_to_end(Dict *d, const char *name) {
    int slot = d_find(d, name);
    if (slot < 0) return 0;
    order_unlink(d, slot);
    order_append(d, slot);
    return 1;
}

/* full audit: every index must describe exactly the live records */
static void d_audit(const Dict *d) {
    int live = 0;
    for (int i = 0; i < d->nslots; i++) live += d->r[i].alive;
    CHECK(live == d->count && d->nscore == d->count);
    int n = 0, prev = -1;
    for (int i = d->order_head; i >= 0; i = d->r[i].order_next) { CHECK(d->r[i].alive && d->r[i].order_prev == prev); prev = i; n++; }
    CHECK(n == d->count && d->order_tail == prev);
    for (int i = 1; i < d->nscore; i++) CHECK(score_less(d, d->by_score[i - 1], d->by_score[i]));
    int gn = 0;
    for (int g = 0; g < NGROUP; g++)
        for (int i = d->grp_head[g]; i >= 0; i = d->r[i].grp_next) { CHECK(d->r[i].alive && d->r[i].group == g); gn++; }
    CHECK(gn == d->count);
    int hn = 0;
    for (int b = 0; b < NBUCKET; b++)
        for (int i = d->bucket[b]; i >= 0; i = d->r[i].hash_next) { CHECK(d->r[i].alive && (int)hname(d->r[i].name) == b); hn++; }
    CHECK(hn == d->count);
}

/* brute-force model: insertion-ordered array of records */
typedef struct { char name[12]; int score, group; } M;

int main(void) {
    static Dict d;
    d_init(&d);
    M m[MAXREC];
    int mn = 0;
    long ins = 0, dup = 0, del = 0, upd = 0, ren = 0, mv = 0;
    for (int step = 0; step < 6000; step++) {
        char name[12];
        snprintf(name, sizeof name, "n%d", (int)(rnd() % 150));
        int idx = -1;
        for (int i = 0; i < mn; i++) if (strcmp(m[i].name, name) == 0) { idx = i; break; }
        unsigned op = rnd() % 12;
        int score = (int)(rnd() % 50), group = (int)(rnd() % NGROUP);
        if (op < 4) {
            int slot = d_insert(&d, name, score, group);
            if (idx >= 0) { CHECK(slot < 0); dup++; }
            else if (mn < MAXREC) {
                CHECK(slot >= 0);
                snprintf(m[mn].name, sizeof m[mn].name, "%s", name); m[mn].score = score; m[mn].group = group; mn++;
                ins++;
            }
        } else if (op < 6) {
            int r = d_erase(&d, name);
            CHECK(r == (idx >= 0));
            if (idx >= 0) { memmove(&m[idx], &m[idx + 1], (size_t)(mn - idx - 1) * sizeof(M)); mn--; del++; }
        } else if (op < 8) {
            CHECK(d_set_score(&d, name, score) == (idx >= 0));
            if (idx >= 0) { m[idx].score = score; upd++; }
        } else if (op < 9) {
            CHECK(d_set_group(&d, name, group) == (idx >= 0));
            if (idx >= 0) m[idx].group = group;
        } else if (op < 10) {
            char to[12];
            snprintf(to, sizeof to, "n%d", (int)(rnd() % 150));
            int tidx = -1;
            for (int i = 0; i < mn; i++) if (strcmp(m[i].name, to) == 0) tidx = i;
            int ok = d_rename(&d, name, to);
            CHECK(ok == (idx >= 0 && tidx < 0));
            if (ok) { snprintf(m[idx].name, sizeof m[idx].name, "%s", to); ren++; }
        } else if (op < 11) {
            CHECK(d_move_to_end(&d, name) == (idx >= 0));
            if (idx >= 0) { M t = m[idx]; memmove(&m[idx], &m[idx + 1], (size_t)(mn - idx - 1) * sizeof(M)); m[mn - 1] = t; mv++; }
        } else {
            int slot = d_find(&d, name);
            CHECK((slot >= 0) == (idx >= 0));
            if (slot >= 0) CHECK(d.r[slot].score == m[idx].score && d.r[slot].group == m[idx].group);
        }
        CHECK(d.count == mn);
        if (step % 200 == 0) d_audit(&d);
    }
    d_audit(&d);
    /* iteration orders against the model */
    int i = 0;
    for (int s = d.order_head; s >= 0; s = d.r[s].order_next, i++) CHECK(strcmp(d.r[s].name, m[i].name) == 0);
    CHECK(i == mn);
    int grp_count[NGROUP] = { 0 }, model_grp[NGROUP] = { 0 };
    for (int g = 0; g < NGROUP; g++) for (int s = d.grp_head[g]; s >= 0; s = d.r[s].grp_next) grp_count[g]++;
    for (int k = 0; k < mn; k++) model_grp[m[k].group]++;
    for (int g = 0; g < NGROUP; g++) CHECK(grp_count[g] == model_grp[g]);
    int top = 0;
    long range_sum = 0;
    for (int k = d.nscore - 1; k >= 0 && top < 3; k--, top++) range_sum += d.r[d.by_score[k]].score;
    printf("records=%d inserts=%ld duplicate rejects=%ld deletes=%ld score updates=%ld renames=%ld moves=%ld\n", d.count, ins, dup, del, upd, ren, mv);
    printf("groups: %d %d %d %d %d %d\n", grp_count[0], grp_count[1], grp_count[2], grp_count[3], grp_count[4], grp_count[5]);
    printf("lowest score %d, highest %d, top-3 sum %ld\n", d.r[d.by_score[0]].score, d.r[d.by_score[d.nscore - 1]].score, range_sum);
    printf("first in insertion order: %s, last: %s\n", d.r[d.order_head].name, d.r[d.order_tail].name);
    return 0;
}
