/*
 * title: Sealed buffers with checked mutation, freeze and thaw generations
 * topic: memory
 * covers: write protection by API, sealed state, generation counters, hash of frozen content, stale-view detection
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t len, cap;
    int frozen;
    unsigned gen;      /* bumps on every thaw so old views can be recognised */
    uint32_t seal_hash;
    long refused_writes;
} SBuf;

typedef struct { const SBuf *b; unsigned gen; } View; /* a read-only view valid for one generation */

static uint32_t fnv(const unsigned char *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int sb_init(SBuf *b, size_t cap) {
    b->data = malloc(cap);
    if (!b->data) return 0;
    b->len = 0; b->cap = cap; b->frozen = 0; b->gen = 1; b->seal_hash = 0; b->refused_writes = 0;
    return 1;
}
static int sb_write(SBuf *b, size_t at, const void *src, size_t n) {
    if (b->frozen) { b->refused_writes++; return -1; }
    if (at > b->cap || n > b->cap - at) return -2;
    memcpy(b->data + at, src, n);
    if (at + n > b->len) b->len = at + n;
    return 0;
}
static void sb_freeze(SBuf *b) { b->seal_hash = fnv(b->data, b->len); b->frozen = 1; }
static int sb_verify(const SBuf *b) { return !b->frozen || fnv(b->data, b->len) == b->seal_hash; }
static void sb_thaw(SBuf *b) { b->frozen = 0; b->gen++; }
static View sb_view(const SBuf *b) { View v = {b, b->gen}; return v; }
static int view_read(View v, size_t at, unsigned char *out) {
    if (v.gen != v.b->gen) return -1;  /* stale after thaw */
    if (at >= v.b->len) return -2;
    *out = v.b->data[at];
    return 0;
}

int main(void) {
    SBuf b;
    if (!sb_init(&b, 32)) return 1;
    printf("write hello: %d\n", sb_write(&b, 0, "hello", 5));
    printf("write world at 5: %d\n", sb_write(&b, 5, " world", 6));
    printf("write past cap: %d\n", sb_write(&b, 30, "xyz", 3));
    printf("len=%zu cap=%zu\n", b.len, b.cap);

    sb_freeze(&b);
    View v = sb_view(&b);
    printf("frozen hash=%08x verify=%d\n", (unsigned)b.seal_hash, sb_verify(&b));
    printf("write while frozen: %d\n", sb_write(&b, 0, "J", 1));
    int again = sb_write(&b, 1, "J", 1);
    printf("write while frozen again: %d refused=%ld\n", again, b.refused_writes);
    unsigned char c;
    int vr = view_read(v, 4, &c);
    printf("view read [4]: %d '%c'\n", vr, c);
    printf("view read [50]: %d\n", view_read(v, 50, &c));

    /* simulate a stray raw write bypassing the API: the seal check notices */
    b.data[2] ^= 0x20;
    printf("after stray write: verify=%d\n", sb_verify(&b));
    b.data[2] ^= 0x20;
    printf("after repair: verify=%d\n", sb_verify(&b));

    sb_thaw(&b);
    printf("view after thaw: %d (stale)\n", view_read(v, 4, &c));
    printf("write after thaw: %d\n", sb_write(&b, 0, "J", 1));
    View v2 = sb_view(&b);
    vr = view_read(v2, 0, &c);
    printf("new view read [0]: %d '%c' gen=%u\n", vr, c, b.gen);
    sb_freeze(&b);

    /* cycle: 100 freeze/thaw generations with a checksum trail */
    uint32_t trail = 0;
    for (int i = 0; i < 100; i++) {
        sb_thaw(&b);
        unsigned char x = (unsigned char)('a' + i % 26);
        sb_write(&b, (size_t)(i % 11), &x, 1);
        sb_freeze(&b);
        trail = trail * 31 + b.seal_hash;
        if (!sb_verify(&b)) return 1;
    }
    printf("generation=%u trail=%08x content=%.11s\n", b.gen, (unsigned)trail, (const char *)b.data);
    free(b.data);
    return 0;
}
