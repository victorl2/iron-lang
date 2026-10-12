/*
 * title: Growable string builder
 * topic: data_structures
 * covers: string builder, append, formatted append, insert, replace, truncation, amortized growth
 * deps: libc
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x2E1B21385C26C926ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 22); }

typedef struct { char *s; size_t len, cap; int grows; } SB;

static void sb_init(SB *b) { b->cap = 16; b->s = malloc(b->cap); CHECK(b->s); b->s[0] = 0; b->len = 0; b->grows = 0; }
static void sb_free(SB *b) { free(b->s); }
static void sb_reserve(SB *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    b->s = realloc(b->s, b->cap); CHECK(b->s); b->grows++;
}
static void sb_append(SB *b, const char *t) { size_t n = strlen(t); sb_reserve(b, n); memcpy(b->s + b->len, t, n + 1); b->len += n; }
static void sb_putc(SB *b, char c) { sb_reserve(b, 1); b->s[b->len++] = c; b->s[b->len] = 0; }
static void sb_printf(SB *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    CHECK(n >= 0);
    sb_reserve(b, (size_t)n);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap2); va_end(ap2);
    b->len += (size_t)n;
}
static void sb_insert(SB *b, size_t pos, const char *t) {
    CHECK(pos <= b->len);
    size_t n = strlen(t); sb_reserve(b, n);
    memmove(b->s + pos + n, b->s + pos, b->len - pos + 1); memcpy(b->s + pos, t, n); b->len += n;
}
static void sb_erase(SB *b, size_t pos, size_t n) {
    CHECK(pos <= b->len); if (n > b->len - pos) n = b->len - pos;
    memmove(b->s + pos, b->s + pos + n, b->len - pos - n + 1); b->len -= n;
}
static size_t sb_replace_all(SB *b, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to), count = 0, pos = 0;
    CHECK(fl > 0);
    for (;;) {
        char *hit = strstr(b->s + pos, from);
        if (!hit) break;
        size_t at = (size_t)(hit - b->s);
        sb_erase(b, at, fl); sb_insert(b, at, to); pos = at + tl; count++;
    }
    return count;
}
static void sb_truncate(SB *b, size_t n) { if (n < b->len) { b->len = n; b->s[n] = 0; } }

/* reference: fixed big buffer with the same operations, written independently */
static char ref[1 << 16]; static size_t rlen;
static void r_append(const char *t) { size_t n = strlen(t); CHECK(rlen + n < sizeof ref); memcpy(ref + rlen, t, n); rlen += n; ref[rlen] = 0; }

int main(void) {
    SB b; sb_init(&b); ref[0] = 0; rlen = 0;
    long cnt[6] = {0};
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        char word[16]; size_t wl = 1 + rnd() % 6;
        for (size_t i = 0; i < wl; i++) word[i] = (char)('a' + rnd() % 4);
        word[wl] = 0;
        if (rlen > 20000) op = 90;
        if (op < 35) { sb_append(&b, word); r_append(word); cnt[0]++; }
        else if (op < 45) { sb_putc(&b, (char)('A' + rnd() % 26)); char t[2] = { b.s[b.len - 1], 0 }; r_append(t); cnt[1]++; }
        else if (op < 60) {
            int v = (int)(rnd() % 100000) - 50000; unsigned u = rnd() % 256;
            char expect[64]; snprintf(expect, sizeof expect, "[%d|%5.2f|%02x]", v, v / 100.0, u);
            sb_printf(&b, "[%d|%5.2f|%02x]", v, v / 100.0, u); r_append(expect); cnt[2]++;
        } else if (op < 75) {
            size_t pos = rnd() % (rlen + 1); sb_insert(&b, pos, word);
            memmove(ref + pos + wl, ref + pos, rlen - pos + 1); memcpy(ref + pos, word, wl); rlen += wl; cnt[3]++;
        } else if (op < 85) {
            size_t pos = rnd() % (rlen + 1), n = rnd() % 10; sb_erase(&b, pos, n);
            if (n > rlen - pos) n = rlen - pos;
            memmove(ref + pos, ref + pos + n, rlen - pos - n + 1); rlen -= n; cnt[4]++;
        } else if (op < 90) {
            char from[3] = { (char)('a' + rnd() % 4), (char)('a' + rnd() % 4), 0 };
            size_t hits = sb_replace_all(&b, from, "xyz");
            size_t h2 = 0; char *p = ref; char out[1 << 16]; size_t ol = 0;
            while (*p) { if (strncmp(p, from, 2) == 0) { memcpy(out + ol, "xyz", 3); ol += 3; p += 2; h2++; } else out[ol++] = *p++; }
            CHECK(ol < sizeof out); memcpy(ref, out, ol); ref[ol] = 0; rlen = ol; CHECK(hits == h2); cnt[5]++;
        } else if (op < 99) { sb_append(&b, word); r_append(word); cnt[0]++;
        } else { size_t keep = rlen / 2; sb_truncate(&b, keep); rlen = keep; ref[rlen] = 0; }
        CHECK(b.len == rlen && strcmp(b.s, ref) == 0);
        CHECK(b.len < b.cap && (b.cap & (b.cap - 1)) == 0);
    }
    printf("append=%ld putc=%ld printf=%ld insert=%ld erase=%ld replace=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    printf("len=%zu cap=%zu grows=%d\n", b.len, b.cap, b.grows);
    printf("head: %.40s\n", b.s);
    sb_free(&b);
    return 0;
}
