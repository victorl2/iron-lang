/*
 * title: Piece table text editor with undo
 * topic: data_structures
 * covers: piece table, original and append buffers, piece splitting, insert/delete ranges, undo snapshots
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x12835B0145706FBEULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 19); }

typedef struct { int src; size_t off, len; } Piece; /* src 0 = original, 1 = added */
typedef struct { Piece *p; size_t n, cap; } Pieces;

#define ORIG_LEN 200
#define ADD_MAX 20000
static char orig[ORIG_LEN + 1];
static char added[ADD_MAX];
static size_t added_len;

static Pieces cur;
static Pieces *undo; static size_t undo_n, undo_cap;

static void pieces_reserve(Pieces *s, size_t n) {
    if (n <= s->cap) return;
    s->cap = s->cap ? s->cap * 2 : 8; while (s->cap < n) s->cap *= 2;
    s->p = realloc(s->p, s->cap * sizeof(Piece)); CHECK(s->p);
}
static void pieces_insert(Pieces *s, size_t at, Piece x) {
    pieces_reserve(s, s->n + 1);
    memmove(s->p + at + 1, s->p + at, (s->n - at) * sizeof(Piece)); s->p[at] = x; s->n++;
}
static void pieces_remove(Pieces *s, size_t at) { memmove(s->p + at, s->p + at + 1, (s->n - at - 1) * sizeof(Piece)); s->n--; }
static size_t text_len(const Pieces *s) { size_t t = 0; for (size_t i = 0; i < s->n; i++) t += s->p[i].len; return t; }

static void snapshot(void) {
    if (undo_n == undo_cap) { undo_cap = undo_cap ? undo_cap * 2 : 8; undo = realloc(undo, undo_cap * sizeof *undo); CHECK(undo); }
    Pieces c = {0}; pieces_reserve(&c, cur.n ? cur.n : 1); memcpy(c.p, cur.p, cur.n * sizeof(Piece)); c.n = cur.n;
    undo[undo_n++] = c;
}
static void undo_last(void) {
    CHECK(undo_n);
    free(cur.p); cur = undo[--undo_n];
}
/* split so that a piece boundary exists at text position pos; returns the index of the piece starting at pos */
static size_t split_at(size_t pos) {
    size_t acc = 0;
    for (size_t i = 0; i < cur.n; i++) {
        if (pos == acc) return i;
        if (pos < acc + cur.p[i].len) {
            size_t k = pos - acc; Piece right = { cur.p[i].src, cur.p[i].off + k, cur.p[i].len - k };
            cur.p[i].len = k; pieces_insert(&cur, i + 1, right); return i + 1;
        }
        acc += cur.p[i].len;
    }
    CHECK(pos == acc); return cur.n;
}
static void insert_text(size_t pos, const char *s, size_t n) {
    CHECK(added_len + n <= ADD_MAX);
    snapshot();
    memcpy(added + added_len, s, n);
    size_t i = split_at(pos);
    Piece x = { 1, added_len, n }; pieces_insert(&cur, i, x); added_len += n;
}
static void delete_range(size_t pos, size_t n) {
    if (!n) return;
    snapshot();
    split_at(pos + n);           /* boundaries at both ends of the range */
    size_t a = split_at(pos);    /* splitting at pos only affects pieces before pos+n's boundary index */
    size_t acc = 0, b = cur.n;
    for (size_t k = 0; k < cur.n; k++) { if (acc == pos + n) { b = k; break; } acc += cur.p[k].len; }
    while (b > a) { pieces_remove(&cur, a); b--; }
}
static void render(const Pieces *s, char *out) {
    for (size_t i = 0; i < s->n; i++) { memcpy(out, (s->p[i].src ? added : orig) + s->p[i].off, s->p[i].len); out += s->p[i].len; }
}

#define MAXT 6000
int main(void) {
    for (int i = 0; i < ORIG_LEN; i++) orig[i] = (char)('A' + i % 26);
    cur.p = NULL; cur.n = cur.cap = 0;
    Piece all = { 0, 0, ORIG_LEN }; pieces_insert(&cur, 0, all);
    static char model[MAXT], out[MAXT]; size_t mn = ORIG_LEN; memcpy(model, orig, ORIG_LEN);
    static char hist[64][MAXT]; static size_t hist_len[64]; size_t hn = 0;
    long ins = 0, del = 0, und = 0; size_t max_pieces = 0;
    for (int step = 0; step < 1500; step++) {
        unsigned op = rnd() % 100;
        if (mn > 2000) op = 60;
        if (op < 50 && added_len < ADD_MAX - 40) {
            size_t pos = rnd() % (mn + 1), n = 1 + rnd() % 12; char s[16];
            for (size_t i = 0; i < n; i++) s[i] = (char)('a' + rnd() % 26);
            if (hn < 64) { memcpy(hist[hn], model, mn); hist_len[hn++] = mn; } else hn = 0;
            insert_text(pos, s, n);
            memmove(model + pos + n, model + pos, mn - pos); memcpy(model + pos, s, n); mn += n; ins++;
            if (hn == 0) { /* history window reset: forget undo snapshots too */ while (undo_n) { free(undo[--undo_n].p); } }
        } else if (op < 85 && mn) {
            size_t pos = rnd() % mn, n = 1 + rnd() % 15; if (n > mn - pos) n = mn - pos;
            if (hn < 64) { memcpy(hist[hn], model, mn); hist_len[hn++] = mn; } else hn = 0;
            delete_range(pos, n);
            memmove(model + pos, model + pos + n, mn - pos - n); mn -= n; del++;
            if (hn == 0) { while (undo_n) { free(undo[--undo_n].p); } }
        } else if (hn > 0 && undo_n > 0) {
            undo_last(); hn--; memcpy(model, hist[hn], hist_len[hn]); mn = hist_len[hn]; und++;
        }
        CHECK(text_len(&cur) == mn);
        render(&cur, out); CHECK(memcmp(out, model, mn) == 0);
        if (cur.n > max_pieces) max_pieces = cur.n;
        for (size_t i = 0; i < cur.n; i++) CHECK(cur.p[i].len > 0);
    }
    render(&cur, out);
    printf("insert=%ld delete=%ld undo=%ld\n", ins, del, und);
    printf("text_len=%zu pieces=%zu max_pieces=%zu added_bytes=%zu undo_depth=%zu\n", mn, cur.n, max_pieces, added_len, undo_n);
    printf("head: %.40s\n", out);
    unsigned h = 2166136261u; for (size_t i = 0; i < mn; i++) h = (h ^ (unsigned char)out[i]) * 16777619u;
    printf("fnv=%u\n", h);
    while (undo_n) free(undo[--undo_n].p);
    free(undo); free(cur.p);
    return 0;
}
