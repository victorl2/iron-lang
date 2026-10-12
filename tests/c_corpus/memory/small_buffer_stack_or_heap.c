/*
 * title: Small-buffer optimization: fixed stack buffer with heap fallback
 * topic: memory
 * covers: alloca-free scratch space, stack buffer up to a threshold, heap spill, ownership flag, cleanup on all paths
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMALL 64

typedef struct {
    unsigned char inline_buf[SMALL];
    unsigned char *p;
    size_t cap;
    int on_heap;
} Scratch;

static long heap_allocs, heap_frees;

static int scratch_init(Scratch *s, size_t n) {
    if (n <= SMALL) { s->p = s->inline_buf; s->cap = SMALL; s->on_heap = 0; return 1; }
    s->p = malloc(n);
    if (!s->p) return 0;
    heap_allocs++;
    s->cap = n; s->on_heap = 1;
    return 1;
}
static void scratch_free(Scratch *s) {
    if (s->on_heap) { free(s->p); heap_frees++; }
    s->p = NULL; s->on_heap = 0;
}
/* grow preserving contents: inline -> heap copies, heap -> heap reallocs safely */
static int scratch_grow(Scratch *s, size_t n) {
    if (n <= s->cap) return 1;
    size_t nc = s->cap * 2 > n ? s->cap * 2 : n;
    if (!s->on_heap) {
        unsigned char *h = malloc(nc);
        if (!h) return 0;
        heap_allocs++;
        memcpy(h, s->inline_buf, s->cap);
        s->p = h; s->on_heap = 1;
    } else {
        unsigned char *h = realloc(s->p, nc);
        if (!h) return 0;
        s->p = h;
    }
    s->cap = nc;
    return 1;
}

/* reverse the words of a sentence using scratch sized to the input; returns 0 on failure */
static int reverse_words(const char *in, char *out, size_t outcap) {
    size_t n = strlen(in);
    Scratch s;
    if (!scratch_init(&s, n + 1)) return 0;
    size_t w = 0;
    /* copy in reverse token order using the scratch as a staging area */
    size_t end = n;
    while (end > 0) {
        while (end > 0 && in[end - 1] == ' ') end--;
        size_t b = end;
        while (b > 0 && in[b - 1] != ' ') b--;
        if (b == end) break;
        if (w) { if (!scratch_grow(&s, w + 1)) { scratch_free(&s); return 0; } s.p[w++] = ' '; }
        if (!scratch_grow(&s, w + (end - b) + 1)) { scratch_free(&s); return 0; }
        memcpy(s.p + w, in + b, end - b);
        w += end - b;
        end = b;
    }
    s.p[w] = '\0';
    int ok = w < outcap;
    if (ok) memcpy(out, s.p, w + 1);
    int heap = s.on_heap;
    scratch_free(&s);
    return ok ? 1 + heap : 0;
}

static unsigned long checksum(const unsigned char *p, size_t n) { unsigned long h = 1469598103u; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511u; return h & 0xFFFFFFFFu; }

int main(void) {
    char out[4096];
    const char *tests[] = {"one two three", "  leading and trailing  ", "single", ""};
    for (size_t i = 0; i < 4; i++) {
        int r = reverse_words(tests[i], out, sizeof out);
        printf("\"%s\" -> \"%s\" (%s)\n", tests[i], out, r == 1 ? "stack" : r == 2 ? "heap" : "fail");
    }

    /* long input forces heap */
    char big[3000];
    size_t pos = 0;
    for (int i = 0; pos < 2500; i++) pos += (size_t)snprintf(big + pos, sizeof big - pos, "w%d ", i);
    int r = reverse_words(big, out, sizeof out);
    printf("long sentence: %zu chars -> %s, first word \"%.6s\"\n", pos, r == 2 ? "heap" : "unexpected", out);
    if (r != 2) return 1;

    /* threshold sweep: sizes around SMALL choose the right storage */
    printf("threshold:");
    for (size_t n = SMALL - 2; n <= SMALL + 2; n++) {
        Scratch s;
        if (!scratch_init(&s, n)) return 1;
        printf(" %zu=%s", n, s.on_heap ? "heap" : "stack");
        scratch_free(&s);
    }
    printf("\n");

    /* growth keeps contents through the stack -> heap -> heap transitions */
    Scratch s;
    scratch_init(&s, 10);
    size_t len = 0;
    unsigned char ref[1000];
    for (int i = 0; i < 1000; i++) {
        if (!scratch_grow(&s, len + 1)) return 1;
        s.p[len] = (unsigned char)(i * 13);
        ref[len] = s.p[len];
        len++;
    }
    printf("grown to cap %zu on_heap=%d intact=%d checksum=%lu\n", s.cap, s.on_heap, memcmp(s.p, ref, len) == 0, checksum(s.p, len));
    scratch_free(&s);
    printf("heap allocs=%ld frees=%ld\n", heap_allocs, heap_frees);
    return 0;
}
