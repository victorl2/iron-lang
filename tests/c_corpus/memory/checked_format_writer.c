/*
 * title: Checked formatting writer with sticky truncation and two-pass sizing
 * topic: memory
 * covers: snprintf return handling, sticky error state, would-have-written count, exact-size second pass
 * deps: libc
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;   /* bytes available including the NUL */
    size_t len;   /* bytes actually stored */
    size_t want;  /* bytes the full output needs (excluding NUL) */
    int err;      /* sticky: encoding error from vsnprintf */
} Fmt;

static void fmt_init(Fmt *f, char *buf, size_t cap) {
    f->buf = buf; f->cap = cap; f->len = 0; f->want = 0; f->err = 0;
    if (cap) buf[0] = '\0';
}

static void fmt_add(Fmt *f, const char *fmt, ...) {
    if (f->err) return;
    va_list ap;
    va_start(ap, fmt);
    size_t room = f->cap > f->len ? f->cap - f->len : 0;
    int n = vsnprintf(room ? f->buf + f->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n < 0) { f->err = 1; return; }
    f->want += (size_t)n;
    if (room) {
        size_t stored = (size_t)n < room ? (size_t)n : room - 1;
        f->len += stored;
    }
}

static int fmt_truncated(const Fmt *f) { return f->want > (f->cap ? f->cap - 1 : 0); }

static void render(Fmt *f, int rows) {
    fmt_add(f, "[report %d rows]\n", rows);
    for (int i = 0; i < rows; i++)
        fmt_add(f, "%3d | %-6s | %8.3f | %#06x\n", i, i % 2 ? "odd" : "even", i * 1.5, (unsigned)(i * 257));
    fmt_add(f, "[end]");
}

int main(void) {
    /* pass 1: measure with a zero-size buffer */
    Fmt probe;
    fmt_init(&probe, NULL, 0);
    render(&probe, 6);
    printf("measured: %zu bytes needed, truncated=%d\n", probe.want, fmt_truncated(&probe));

    /* pass 2: allocate exactly and render */
    char *exact = malloc(probe.want + 1);
    if (!exact) return 1;
    Fmt f;
    fmt_init(&f, exact, probe.want + 1);
    render(&f, 6);
    if (fmt_truncated(&f) || f.len != probe.want || strlen(exact) != probe.want) { fprintf(stderr, "exact pass failed\n"); return 1; }
    printf("%s\n", exact);

    /* every smaller capacity: never overruns, always terminated, prefix property holds */
    int ok_caps = 0;
    for (size_t cap = 0; cap <= probe.want + 2; cap++) {
        char *b = malloc(cap ? cap : 1);
        if (!b) return 1;
        Fmt g;
        fmt_init(&g, b, cap);
        render(&g, 6);
        if (g.want != probe.want) { fprintf(stderr, "want differs at cap %zu\n", cap); return 1; }
        if (cap) {
            size_t expect = probe.want < cap - 1 ? probe.want : cap - 1;
            if (g.len != expect || strlen(b) != expect || memcmp(b, exact, expect) != 0) { fprintf(stderr, "prefix bad cap %zu\n", cap); return 1; }
        }
        if (fmt_truncated(&g) != (probe.want + 1 > cap)) { fprintf(stderr, "truncation flag bad at %zu\n", cap); return 1; }
        free(b);
        ok_caps++;
    }
    printf("validated %d capacities from 0 to %zu\n", ok_caps, probe.want + 2);

    /* small fixed buffer: report truncation */
    char small[40];
    Fmt s;
    fmt_init(&s, small, sizeof small);
    render(&s, 6);
    printf("small buffer: stored=%zu wanted=%zu truncated=%d first line=\"%.15s\"\n", s.len, s.want, fmt_truncated(&s), small);
    free(exact);
    return 0;
}
