/*
 * title: String builder with overflow-proof growth and hard ceilings
 * topic: memory
 * covers: capacity doubling with overflow checks, configurable ceiling, saturating error state, append variants, realloc-safe growth
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *s;
    size_t len, cap, ceiling;
    int failed; /* sticky */
    int grows;
} SB;

static void sb_init(SB *b, size_t ceiling) { b->s = NULL; b->len = b->cap = 0; b->ceiling = ceiling; b->failed = 0; b->grows = 0; }
static void sb_free(SB *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

/* need = bytes required including the NUL */
static int sb_grow(SB *b, size_t need) {
    if (need <= b->cap) return 1;
    if (need > b->ceiling) return 0;
    size_t nc = b->cap ? b->cap : 8;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) { nc = need; break; }  /* doubling would wrap: fall back to exact */
        nc *= 2;
    }
    if (nc > b->ceiling) nc = b->ceiling;              /* clamp, never exceeds the ceiling */
    char *ns = realloc(b->s, nc);
    if (!ns) return 0;                                 /* old block still owned by b */
    b->s = ns; b->cap = nc; b->grows++;
    return 1;
}

static int sb_append(SB *b, const char *src, size_t n) {
    if (b->failed) return 0;
    if (n > SIZE_MAX - b->len - 1) { b->failed = 1; return 0; }  /* len + n + 1 would wrap */
    if (!sb_grow(b, b->len + n + 1)) { b->failed = 1; return 0; }
    memcpy(b->s + b->len, src, n);
    b->len += n;
    b->s[b->len] = '\0';
    return 1;
}
static int sb_puts(SB *b, const char *s) { return sb_append(b, s, strlen(s)); }
static int sb_putc(SB *b, char c) { return sb_append(b, &c, 1); }

static int sb_repeat(SB *b, char c, size_t times) {
    if (b->failed) return 0;
    if (times > SIZE_MAX - b->len - 1) { b->failed = 1; return 0; }
    if (!sb_grow(b, b->len + times + 1)) { b->failed = 1; return 0; }
    memset(b->s + b->len, c, times);
    b->len += times;
    b->s[b->len] = '\0';
    return 1;
}

static int sb_insert(SB *b, size_t at, const char *src) {
    size_t n = strlen(src);
    if (b->failed || at > b->len) return 0;
    if (n > SIZE_MAX - b->len - 1) return 0;
    if (!sb_grow(b, b->len + n + 1)) { b->failed = 1; return 0; }
    memmove(b->s + at + n, b->s + at, b->len - at + 1);
    memcpy(b->s + at, src, n);
    b->len += n;
    return 1;
}

int main(void) {
    SB b;
    sb_init(&b, 64);
    sb_puts(&b, "hello");
    sb_putc(&b, ',');
    sb_putc(&b, ' ');
    sb_puts(&b, "world");
    printf("\"%s\" len=%zu cap=%zu grows=%d\n", b.s, b.len, b.cap, b.grows);
    sb_insert(&b, 5, "!!!");
    sb_insert(&b, 0, ">> ");
    printf("\"%s\" len=%zu\n", b.s, b.len);
    printf("bad insert position: %d\n", sb_insert(&b, 999, "x"));

    /* fill up to exactly the ceiling: 63 chars + NUL fit, 64 do not */
    size_t room = 64 - 1 - b.len;
    int rep = sb_repeat(&b, '.', room);
    printf("repeat %zu: %d, len=%zu cap=%zu\n", room, rep, b.len, b.cap);
    int more = sb_putc(&b, '#');
    printf("one more byte: %d failed=%d len=%zu\n", more, b.failed, b.len);
    if (b.len != 63 || strlen(b.s) != 63) return 1;
    printf("content unchanged after failure: %d\n", b.s[62] == '.' && b.s[63] == '\0');
    sb_free(&b);

    /* growth schedule under a large ceiling */
    sb_init(&b, (size_t)1 << 20);
    size_t last_cap = 0;
    printf("caps:");
    for (int i = 0; i < 5000; i++) {
        sb_putc(&b, (char)('a' + i % 26));
        if (b.cap != last_cap) { printf(" %zu", b.cap); last_cap = b.cap; }
    }
    printf("\nlen=%zu grows=%d\n", b.len, b.grows);
    for (size_t i = 0; i < b.len; i++) if (b.s[i] != (char)('a' + i % 26)) return 1;
    sb_free(&b);

    /* size_t wrap attempts must fail cleanly, not allocate small */
    sb_init(&b, SIZE_MAX);
    sb_puts(&b, "seed");
    int r1 = sb_repeat(&b, 'x', SIZE_MAX - 2);
    printf("repeat SIZE_MAX-2: %d failed=%d len=%zu s=%s\n", r1, b.failed, b.len, b.s);
    sb_free(&b);
    sb_init(&b, (size_t)1 << 16);
    int r2 = sb_repeat(&b, 'y', SIZE_MAX);
    printf("repeat SIZE_MAX: %d failed=%d cap=%zu\n", r2, b.failed, b.cap);
    sb_free(&b);
    return 0;
}
