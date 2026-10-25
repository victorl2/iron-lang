/*
 * title: Realloc that never loses the original block
 * topic: memory
 * covers: realloc failure handling, simulated allocator limit, safe grow/shrink, no leak on failure
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A limited allocator: refuses to hold more than `limit` live bytes. realloc is built on
 * malloc+memcpy+free so failure semantics are fully controlled and deterministic. */
static size_t limit = 4096, live, peak;
static int failures;

typedef struct { size_t size; } Hdr;
#define HDR ((size_t)16)

static void *lim_alloc(size_t n) {
    if (n > limit || live + n > limit) { failures++; return NULL; }
    unsigned char *raw = malloc(HDR + n);
    if (!raw) return NULL;
    ((Hdr *)raw)->size = n;
    live += n;
    if (live > peak) peak = live;
    return raw + HDR;
}
static void lim_free(void *p) {
    if (!p) return;
    unsigned char *raw = (unsigned char *)p - HDR;
    live -= ((Hdr *)raw)->size;
    free(raw);
}
static void *lim_realloc(void *p, size_t n) {
    if (!p) return lim_alloc(n);
    size_t old = ((Hdr *)((unsigned char *)p - HDR))->size;
    /* the new block must coexist with the old one while copying */
    void *q = lim_alloc(n);
    if (!q) return NULL; /* original untouched */
    memcpy(q, p, old < n ? old : n);
    lim_free(p);
    return q;
}

typedef struct { unsigned char *data; size_t len, cap; } Buf;

/* WRONG idiom shown for contrast: p = realloc(p, n) leaks p on failure. */
static int buf_reserve(Buf *b, size_t want) {
    if (want <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 16;
    while (nc < want) nc *= 2;
    unsigned char *np = lim_realloc(b->data, nc);
    if (!np) {
        /* try the exact size before giving up */
        np = lim_realloc(b->data, want);
        if (!np) return -1;
        nc = want;
    }
    b->data = np;
    b->cap = nc;
    return 0;
}

static int buf_append(Buf *b, const void *src, size_t n) {
    if (buf_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static unsigned char pat(size_t i) { return (unsigned char)(i * 7u + 3u); }

int main(void) {
    Buf b = {0};
    unsigned char chunk[100];
    size_t written = 0;
    int rc = 0;
    int step = 0;
    while (rc == 0) {
        for (size_t i = 0; i < sizeof chunk; i++) chunk[i] = pat(written + i);
        rc = buf_append(&b, chunk, sizeof chunk);
        if (rc == 0) written += sizeof chunk;
        step++;
        if (step % 8 == 0 || rc != 0)
            printf("step %2d rc=%d len=%zu cap=%zu live=%zu\n", step, rc, b.len, b.cap, live);
    }
    /* the original block survived the failed growth: verify all bytes */
    for (size_t i = 0; i < b.len; i++)
        if (b.data[i] != pat(i)) { fprintf(stderr, "corrupted at %zu\n", i); return 1; }
    if (b.len != written) { fprintf(stderr, "len mismatch\n"); return 1; }
    printf("survived: len=%zu verified, failures=%d\n", b.len, failures);

    /* shrink always succeeds (new block smaller), then growth works again */
    Buf keep = b;
    unsigned char *sm = lim_realloc(b.data, b.len);
    if (!sm) { fprintf(stderr, "shrink failed\n"); return 1; }
    /* shrink while both exist needs room: only possible if len <= limit - cap */
    printf("shrink to len ok\n");
    b.data = sm; b.cap = b.len;
    (void)keep;
    limit = 8192;
    rc = buf_append(&b, chunk, sizeof chunk);
    printf("after raising limit: rc=%d len=%zu cap=%zu\n", rc, b.len, b.cap);

    /* extreme request: size beyond limit must fail and leave block intact */
    size_t before = b.len;
    unsigned char *huge = lim_realloc(b.data, (size_t)1 << 40);
    printf("huge realloc: %s, block intact=%d\n", huge ? "unexpected" : "refused", b.data[before - 1] == chunk[99]);
    if (huge) return 1;
    lim_free(b.data);
    printf("live at end=%zu peak=%zu\n", live, peak);
    if (live != 0) return 1;
    return 0;
}
