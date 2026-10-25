/*
 * title: Small-string optimization with inline and heap representations
 * topic: memory
 * covers: SSO, union of representations, growth, shrink-to-inline, reference model cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INLINE_CAP 23

/*
 * 24 bytes total. Inline: 23 chars + a final byte holding the length (0..22).
 * Heap: pointer, length, 32-bit capacity, padding, and a final marker byte 0xFF.
 * The mode test reads only that final byte, so it does not depend on endianness.
 */
typedef struct {
    union {
        struct {
            char *ptr;
            size_t len;
            uint32_t cap;
            unsigned char pad[3];
            unsigned char marker; /* 0xFF when on the heap */
        } heap;
        struct {
            char buf[INLINE_CAP];
            unsigned char len_byte; /* 0..22 when inline */
        } sso;
    } u;
} Str;

#define HEAP_MARK 0xFFu

static unsigned long n_heap_allocs, n_promotions, n_demotions;

static int is_heap(const Str *s) {
    return s->u.sso.len_byte == HEAP_MARK;
}

static void str_init(Str *s) {
    memset(s, 0, sizeof *s);
}

static const char *str_data(const Str *s) {
    return is_heap(s) ? s->u.heap.ptr : s->u.sso.buf;
}

static size_t str_len(const Str *s) {
    return is_heap(s) ? s->u.heap.len : s->u.sso.len_byte;
}

static size_t str_cap(const Str *s) {
    return is_heap(s) ? s->u.heap.cap : INLINE_CAP - 1;
}

static void str_free(Str *s) {
    if (is_heap(s))
        free(s->u.heap.ptr);
    str_init(s);
}

static void str_reserve(Str *s, size_t want) {
    if (want <= str_cap(s))
        return;
    size_t cap = str_cap(s) * 2;
    if (cap < want)
        cap = want;
    char *p = malloc(cap + 1);
    if (!p)
        exit(2);
    n_heap_allocs++;
    size_t len = str_len(s);
    memcpy(p, str_data(s), len + 1);
    if (is_heap(s))
        free(s->u.heap.ptr);
    else
        n_promotions++;
    s->u.heap.ptr = p;
    s->u.heap.len = len;
    s->u.heap.cap = (uint32_t)cap;
    s->u.heap.marker = HEAP_MARK;
}

static void set_len(Str *s, size_t len) {
    if (is_heap(s)) {
        s->u.heap.len = len;
        s->u.heap.ptr[len] = 0;
    } else {
        s->u.sso.len_byte = (unsigned char)len;
        s->u.sso.buf[len] = 0; /* len <= 22 keeps the NUL inside buf */
    }
}

static void str_append(Str *s, const char *t, size_t n) {
    size_t len = str_len(s);
    /* inline mode reserves one byte for the NUL, so usable inline capacity is 22 */
    size_t need = len + n;
    if (!is_heap(s) && need > INLINE_CAP - 1)
        str_reserve(s, need < 2 * INLINE_CAP ? 2 * INLINE_CAP : need);
    else if (is_heap(s) && need > str_cap(s))
        str_reserve(s, need);
    char *d = is_heap(s) ? s->u.heap.ptr : s->u.sso.buf;
    memcpy(d + len, t, n);
    set_len(s, need);
}

/* shrink back to the inline form when the contents fit again */
static void str_shrink(Str *s) {
    if (!is_heap(s) || s->u.heap.len > INLINE_CAP - 1)
        return;
    char tmp[INLINE_CAP];
    size_t len = s->u.heap.len;
    memcpy(tmp, s->u.heap.ptr, len);
    free(s->u.heap.ptr);
    memset(s, 0, sizeof *s);
    memcpy(s->u.sso.buf, tmp, len);
    s->u.sso.len_byte = (unsigned char)len;
    n_demotions++;
}

static void str_truncate(Str *s, size_t n) {
    if (n < str_len(s))
        set_len(s, n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t rs = 2024;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

int main(void) {
    check(sizeof(Str) == 24, "24 bytes");
    Str s;
    str_init(&s);
    check(!is_heap(&s) && str_len(&s) == 0, "empty is inline");

    /* deterministic walk across the boundary */
    printf("length -> mode as 'x' is appended:\n");
    for (int i = 1; i <= 26; i++) {
        str_append(&s, "x", 1);
        if (i == 1 || i == 21 || i == 22 || i == 23 || i == 24 || i == 26)
            printf("  len %2zu mode %s\n", str_len(&s), is_heap(&s) ? "heap" : "inline");
    }
    str_truncate(&s, 10);
    printf("after truncate to 10: %s\n", is_heap(&s) ? "heap" : "inline");
    str_shrink(&s);
    printf("after shrink: %s len %zu\n", is_heap(&s) ? "heap" : "inline", str_len(&s));
    check(strcmp(str_data(&s), "xxxxxxxxxx") == 0, "content after shrink");
    str_free(&s);

    /* random operations against a plain reference buffer */
    char ref[600];
    size_t rlen = 0;
    ref[0] = 0;
    str_init(&s);
    unsigned long inline_steps = 0, heap_steps = 0;
    for (int step = 0; step < 4000; step++) {
        uint32_t r = rnd();
        uint32_t op = r % 10;
        if (op < 6) {
            char piece[12];
            size_t n = 1 + (r >> 8) % 11;
            for (size_t i = 0; i < n; i++)
                piece[i] = (char)('a' + (r >> (12 + i)) % 26);
            if (rlen + n < sizeof ref - 1) {
                str_append(&s, piece, n);
                memcpy(ref + rlen, piece, n);
                rlen += n;
                ref[rlen] = 0;
            }
        } else if (op < 9) {
            size_t n = (r >> 8) % (rlen + 1);
            str_truncate(&s, n);
            rlen = n;
            ref[rlen] = 0;
            if (op == 8)
                str_shrink(&s);
        } else {
            str_free(&s);
            rlen = 0;
            ref[0] = 0;
        }
        check(str_len(&s) == rlen, "length matches");
        check(memcmp(str_data(&s), ref, rlen + 1) == 0, "contents match");
        if (is_heap(&s))
            heap_steps++;
        else
            inline_steps++;
    }
    printf("steps inline=%lu heap=%lu\n", inline_steps, heap_steps);
    printf("heap allocations=%lu promotions=%lu demotions=%lu\n", n_heap_allocs, n_promotions, n_demotions);
    str_free(&s);
    return 0;
}
