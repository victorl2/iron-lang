/*
 * title: Own strlcpy and strlcat with truncation reporting
 * topic: memory
 * covers: bounded string copy, truncation detection, return-value semantics, sentinel-guarded buffers, exhaustive sizes
 * deps: libc
 */
#include <stdio.h>
#include <string.h>

/* Semantics (BSD): return the length the result would have had without truncation. */
static size_t my_strlcpy(char *dst, const char *src, size_t size) {
    size_t n = 0;
    while (src[n]) n++;
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        for (size_t i = 0; i < c; i++) dst[i] = src[i];
        dst[c] = '\0';
    }
    return n;
}

static size_t my_strlcat(char *dst, const char *src, size_t size) {
    size_t dl = 0;
    while (dl < size && dst[dl]) dl++;
    if (dl == size) { /* dst not terminated within size: never touch it */
        return size + strlen(src);
    }
    return dl + my_strlcpy(dst + dl, src, size - dl);
}

#define SENT 0x7E
#define PAD 8

/* checks: canary bytes around the buffer stay intact, result is terminated, prefix matches */
static int check_copy(const char *src, size_t size) {
    unsigned char raw[PAD + 64 + PAD];
    memset(raw, SENT, sizeof raw);
    char *dst = (char *)raw + PAD;
    size_t r = my_strlcpy(dst, src, size);
    size_t sl = strlen(src);
    if (r != sl) return 1;
    for (size_t i = 0; i < PAD; i++) if (raw[i] != SENT || raw[PAD + size + i] != SENT) return 2;
    if (size > 0) {
        size_t want = sl < size - 1 ? sl : size - 1;
        if (strlen(dst) != want || memcmp(dst, src, want) != 0) return 3;
    }
    return 0;
}

static int check_cat(const char *a, const char *b, size_t size) {
    unsigned char raw[PAD + 64 + PAD];
    memset(raw, SENT, sizeof raw);
    char *dst = (char *)raw + PAD;
    if (strlen(a) + 1 > size) return -1; /* precondition: initial content fits */
    strcpy(dst, a);
    size_t r = my_strlcat(dst, b, size);
    if (r != strlen(a) + strlen(b)) return 1;
    for (size_t i = 0; i < PAD; i++) if (raw[i] != SENT || raw[PAD + size + i] != SENT) return 2;
    char want[128];
    snprintf(want, sizeof want, "%s%s", a, b);
    if (size > 0 && strncmp(dst, want, size - 1) != 0) return 3;
    if (strlen(dst) > size - 1) return 4;
    return 0;
}

int main(void) {
    const char *srcs[] = {"", "a", "hello", "0123456789abcdef", "the quick brown fox jumps over"};
    int copy_cases = 0, trunc = 0;
    for (size_t si = 0; si < 5; si++)
        for (size_t size = 0; size <= 40; size++) {
            int e = check_copy(srcs[si], size);
            if (e) { fprintf(stderr, "copy fail src=%zu size=%zu code=%d\n", si, size, e); return 1; }
            copy_cases++;
            if (size > 0 && strlen(srcs[si]) >= size) trunc++;
        }
    printf("strlcpy: %d cases, %d truncated, canaries intact\n", copy_cases, trunc);

    int cat_cases = 0, cat_trunc = 0;
    for (size_t ai = 0; ai < 5; ai++)
        for (size_t bi = 0; bi < 5; bi++)
            for (size_t size = 1; size <= 60; size++) {
                int e = check_cat(srcs[ai], srcs[bi], size);
                if (e == -1) continue;
                if (e) { fprintf(stderr, "cat fail %zu %zu size=%zu code=%d\n", ai, bi, size, e); return 1; }
                cat_cases++;
                if (strlen(srcs[ai]) + strlen(srcs[bi]) >= size) cat_trunc++;
            }
    printf("strlcat: %d cases, %d truncated, canaries intact\n", cat_cases, cat_trunc);

    /* unterminated destination must not be written */
    char un[4] = {'a', 'b', 'c', 'd'};
    size_t r = my_strlcat(un, "xyz", sizeof un);
    printf("unterminated dst: ret=%zu bytes=%c%c%c%c\n", r, un[0], un[1], un[2], un[3]);

    /* idiom: build a path in 16 bytes and detect truncation by comparing the return value */
    char path[16];
    size_t need = my_strlcpy(path, "/usr/local", sizeof path);
    need = my_strlcat(path, "/share/doc", sizeof path);
    printf("path=\"%s\" needed=%zu truncated=%d\n", path, need, need >= sizeof path);
    char big[32];
    need = my_strlcpy(big, "/usr/local", sizeof big);
    need = my_strlcat(big, "/share/doc", sizeof big);
    printf("path=\"%s\" needed=%zu truncated=%d\n", big, need, need >= sizeof big);
    return 0;
}
