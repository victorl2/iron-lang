/*
 * title: Shadow-memory addressability map catching heap misuse
 * topic: memory
 * covers: shadow bytes, 8-byte granules with partial addressability, redzones, use-after-free, invalid access report classes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A simulated 4 KB heap with a shadow byte per 8-byte granule (like ASan):
 * 0 = all 8 addressable, 1..7 = first k bytes addressable, negative markers for redzone/freed/unallocated. */
#define HEAP 4096
#define GR 8
enum { S_UNALLOC = -1, S_REDZONE = -2, S_FREED = -3 };
static signed char shadow[HEAP / GR];
static size_t bump = 0;

typedef struct { size_t off, size; int live; } Alloc;
static Alloc allocs[64];
static int nalloc;

static void poison(size_t off, size_t len, signed char v) {
    for (size_t g = off / GR; g < (off + len + GR - 1) / GR; g++) shadow[g] = v;
}

static int h_alloc(size_t n) {
    size_t rz = 16;
    size_t payload = (n + GR - 1) / GR * GR;
    if (bump + rz + payload + rz > HEAP) return -1;
    size_t off = bump + rz;
    poison(bump, rz, S_REDZONE);
    for (size_t g = off / GR; g < (off + payload) / GR; g++) shadow[g] = 0;
    if (n % GR) shadow[(off + n) / GR] = (signed char)(n % GR);
    poison(off + payload, rz, S_REDZONE);
    bump = off + payload + rz;
    allocs[nalloc].off = off; allocs[nalloc].size = n; allocs[nalloc].live = 1;
    return nalloc++;
}

static void h_free(int id) {
    Alloc *a = &allocs[id];
    a->live = 0;
    poison(a->off, (a->size + GR - 1) / GR * GR, S_FREED);
}

/* check whether [off, off+len) is fully addressable; return the class of the first bad byte or 0 */
static int check(size_t off, size_t len) {
    for (size_t i = off; i < off + len; i++) {
        signed char sv = shadow[i / GR];
        if (sv < 0) return sv;
        if (sv > 0 && (i % GR) >= (size_t)sv) return S_REDZONE; /* partial granule tail = overflow into padding */
    }
    return 0;
}

static const char *cls(int c) {
    return c == 0 ? "ok" : c == S_REDZONE ? "overflow" : c == S_FREED ? "use-after-free" : "wild";
}

int main(void) {
    memset(shadow, S_UNALLOC, sizeof shadow);
    int a = h_alloc(13), b = h_alloc(32), c = h_alloc(5);
    if (a < 0 || b < 0 || c < 0) return 1;
    printf("a: off=%zu size=%zu | b: off=%zu size=%zu | c: off=%zu size=%zu\n",
           allocs[a].off, allocs[a].size, allocs[b].off, allocs[b].size, allocs[c].off, allocs[c].size);

    struct { const char *what; int id; long rel; size_t len; } probes[] = {
        {"a[0..12]", 0, 0, 13}, {"a[0..13]", 0, 0, 14}, {"a[12]", 0, 12, 1}, {"a[13]", 0, 13, 1},
        {"a[-1]", 0, -1, 1}, {"b whole", 1, 0, 32}, {"b[31..32]", 1, 31, 2}, {"b[-8..0]", 1, -8, 8},
        {"c[0..4]", 2, 0, 5}, {"c[4..6]", 2, 4, 2}, {"c[5]", 2, 5, 1},
    };
    int nprobe = (int)(sizeof probes / sizeof probes[0]);
    int bad = 0;
    for (int i = 0; i < nprobe; i++) {
        size_t off = (size_t)((long)allocs[probes[i].id].off + probes[i].rel);
        int r = check(off, probes[i].len);
        printf("%-10s -> %s\n", probes[i].what, cls(r));
        bad += r != 0;
    }
    h_free(b);
    printf("after free(b): b[0] -> %s, b[16..24] -> %s, a[0] -> %s\n", cls(check(allocs[b].off, 1)), cls(check(allocs[b].off + 16, 8)), cls(check(allocs[a].off, 1)));
    printf("never-allocated tail: %s\n", cls(check(HEAP - 8, 4)));

    /* brute-force cross-check of every byte offset in a and c against the model */
    int mismatches = 0;
    for (int id = 0; id < 3; id++)
        for (size_t o = allocs[id].off - 16; o < allocs[id].off + allocs[id].size + 16; o++) {
            int inside = o >= allocs[id].off && o < allocs[id].off + allocs[id].size;
            int r = check(o, 1);
            if (!allocs[id].live) inside = 0;
            if (id == 1) continue;
            if ((r == 0) != inside) mismatches++;
        }
    printf("bad probes=%d mismatches=%d\n", bad, mismatches);
    return mismatches == 0 ? 0 : 1;
}
