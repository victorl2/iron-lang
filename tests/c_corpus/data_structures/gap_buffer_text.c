/*
 * title: Gap buffer text editor core
 * topic: data_structures
 * covers: gap buffer, cursor movement, insert/delete at cursor, gap growth, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x6A09E667F3BCC908ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 17); }

typedef struct { char *buf; size_t cap, gs, ge; long moves, grows; } Gap; /* gap is [gs, ge) */

static void gap_init(Gap *g, size_t cap) { g->buf = malloc(cap); CHECK(g->buf); g->cap = cap; g->gs = 0; g->ge = cap; g->moves = g->grows = 0; }
static size_t gap_len(const Gap *g) { return g->cap - (g->ge - g->gs); }
static size_t gap_cursor(const Gap *g) { return g->gs; }
static void gap_move(Gap *g, size_t pos) {
    CHECK(pos <= gap_len(g));
    if (pos < g->gs) {
        size_t n = g->gs - pos;
        memmove(g->buf + g->ge - n, g->buf + pos, n);
        g->gs -= n; g->ge -= n; g->moves += (long)n;
    } else if (pos > g->gs) {
        size_t n = pos - g->gs;
        memmove(g->buf + g->gs, g->buf + g->ge, n);
        g->gs += n; g->ge += n; g->moves += (long)n;
    }
}
static void gap_ensure(Gap *g, size_t need) {
    if (g->ge - g->gs >= need) return;
    size_t ncap = g->cap * 2; while (ncap - gap_len(g) < need) ncap *= 2;
    char *nb = malloc(ncap); CHECK(nb);
    size_t tail = g->cap - g->ge;
    memcpy(nb, g->buf, g->gs);
    memcpy(nb + ncap - tail, g->buf + g->ge, tail);
    free(g->buf);
    g->buf = nb; g->ge = ncap - tail; g->cap = ncap; g->grows++;
}
static void gap_insert(Gap *g, const char *s, size_t n) { gap_ensure(g, n); memcpy(g->buf + g->gs, s, n); g->gs += n; }
static void gap_backspace(Gap *g, size_t n) { CHECK(n <= g->gs); g->gs -= n; }
static void gap_delete(Gap *g, size_t n) { CHECK(n <= g->cap - g->ge); g->ge += n; }
static char gap_at(const Gap *g, size_t i) { return i < g->gs ? g->buf[i] : g->buf[i + (g->ge - g->gs)]; }
static void gap_copy(const Gap *g, char *out) { for (size_t i = 0, n = gap_len(g); i < n; i++) out[i] = gap_at(g, i); }

#define MAXT 8192
static char model[MAXT + 64];
static size_t mlen, mcur;

int main(void) {
    Gap g; gap_init(&g, 8);
    long cnt[5] = {0};
    long total_inserted = 0;
    for (int step = 0; step < 20000; step++) {
        unsigned op = rnd() % 100;
        if (mlen > 4000) op = 85;
        if (op < 45) {
            char s[12]; size_t n = 1 + rnd() % 10;
            for (size_t i = 0; i < n; i++) s[i] = (char)('a' + rnd() % 26);
            gap_insert(&g, s, n);
            memmove(model + mcur + n, model + mcur, mlen - mcur); memcpy(model + mcur, s, n); mcur += n; mlen += n;
            cnt[0]++; total_inserted += (long)n;
        } else if (op < 60) {
            /* local edits dominate real usage: small cursor moves */
            long d = (long)(rnd() % 21) - 10; long np = (long)mcur + d;
            if (np < 0) np = 0;
            if (np > (long)mlen) np = (long)mlen;
            gap_move(&g, (size_t)np); mcur = (size_t)np; cnt[1]++;
        } else if (op < 68) {
            size_t np = rnd() % (mlen + 1); gap_move(&g, np); mcur = np; cnt[2]++;
        } else if (op < 84 && mcur) {
            size_t n = 1 + rnd() % 5; if (n > mcur) n = mcur;
            gap_backspace(&g, n); memmove(model + mcur - n, model + mcur, mlen - mcur); mcur -= n; mlen -= n; cnt[3]++;
        } else if (mlen > mcur) {
            size_t n = 1 + rnd() % 5; if (n > mlen - mcur) n = mlen - mcur;
            gap_delete(&g, n); memmove(model + mcur, model + mcur + n, mlen - mcur - n); mlen -= n; cnt[4]++;
        }
        CHECK(gap_len(&g) == mlen && gap_cursor(&g) == mcur);
        if (step % 50 == 0) {
            char out[MAXT + 64]; gap_copy(&g, out);
            CHECK(memcmp(out, model, mlen) == 0);
        }
    }
    printf("insert=%ld near_move=%ld far_move=%ld backspace=%ld delete=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4]);
    printf("len=%zu cursor=%zu cap=%zu grows=%ld moved_bytes=%ld inserted=%ld\n", gap_len(&g), gap_cursor(&g), g.cap, g.grows, g.moves, total_inserted);
    char out[MAXT + 64]; gap_copy(&g, out);
    printf("text head: %.30s\n", out);
    unsigned h = 2166136261u; for (size_t i = 0; i < mlen; i++) h = (h ^ (unsigned char)out[i]) * 16777619u;
    printf("fnv=%u\n", h);
    free(g.buf);
    return 0;
}
