/*
 * title: Checked memcpy and memmove against a block registry
 * topic: memory
 * covers: range validation, overlap detection, registry of live blocks, refusal codes, destination and source containment
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *base; size_t size; int live; } Block;
static Block reg[16];

static unsigned char *r_alloc(size_t n) {
    for (int i = 0; i < 16; i++)
        if (!reg[i].live) {
            reg[i].base = malloc(n);
            if (!reg[i].base) return NULL;
            reg[i].size = n; reg[i].live = 1;
            memset(reg[i].base, 0, n);
            return reg[i].base;
        }
    return NULL;
}
static void r_free(unsigned char *p) {
    for (int i = 0; i < 16; i++) if (reg[i].live && reg[i].base == p) { reg[i].live = 0; free(p); return; }
}

/* find the block containing p (comparing addresses as integers to avoid relational compares on unrelated objects) */
static const Block *find(const void *p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < 16; i++)
        if (reg[i].live && a >= (uintptr_t)reg[i].base && a < (uintptr_t)reg[i].base + reg[i].size) return &reg[i];
    return NULL;
}

enum { C_OK, C_DST_UNKNOWN, C_SRC_UNKNOWN, C_DST_OVERFLOW, C_SRC_OVERREAD, C_OVERLAP, C_LEN_WRAP };
static const char *cname[] = {"ok", "dst-unknown", "src-unknown", "dst-overflow", "src-overread", "overlap", "len-wrap"};

static int contained(const void *p, size_t n, const Block **bout) {
    const Block *b = find(p);
    if (!b) return -1;
    size_t off = (size_t)((const unsigned char *)p - b->base);
    *bout = b;
    return n <= b->size - off;
}

static int safe_copy(void *dst, const void *src, size_t n, int allow_overlap) {
    const Block *db, *sb;
    if (n == 0) return C_OK;
    uintptr_t d = (uintptr_t)dst, s = (uintptr_t)src;
    if (n > UINTPTR_MAX - d || n > UINTPTR_MAX - s) return C_LEN_WRAP;
    int rd = contained(dst, n, &db);
    if (rd < 0) return C_DST_UNKNOWN;
    int rs = contained(src, n, &sb);
    if (rs < 0) return C_SRC_UNKNOWN;
    if (!rd) return C_DST_OVERFLOW;
    if (!rs) return C_SRC_OVERREAD;
    int overlap = d < s + n && s < d + n;
    if (overlap && !allow_overlap) return C_OVERLAP;
    if (allow_overlap) memmove(dst, src, n); else memcpy(dst, src, n);
    return C_OK;
}

int main(void) {
    unsigned char *a = r_alloc(32), *b = r_alloc(16);
    if (!a || !b) return 1;
    for (int i = 0; i < 32; i++) a[i] = (unsigned char)(i + 1);
    unsigned char stack_var[8] = {0};

    struct { const char *what; unsigned char *d; const unsigned char *s; size_t n; int ov; int want; } t[] = {
        {"a[0..8]->b", b, a, 8, 0, C_OK},
        {"a[0..16]->b", b, a, 16, 0, C_OK},
        {"a[0..17]->b", b, a, 17, 0, C_DST_OVERFLOW},
        {"b[10..]x9->a", a, b + 10, 9, 0, C_SRC_OVERREAD},
        {"a[24..]<-a x9", a + 24, a, 9, 0, C_DST_OVERFLOW},
        {"stack->b", b, stack_var, 4, 0, C_SRC_UNKNOWN},
        {"a->stack", stack_var, a, 4, 0, C_DST_UNKNOWN},
        {"a[0..8]->a[4..]", a + 4, a, 8, 0, C_OVERLAP},
        {"memmove a[0..8]->a[4..]", a + 4, a, 8, 1, C_OK},
        {"a[16..24]->a[0..8]", a, a + 16, 8, 0, C_OK},
        {"huge len", b, a, SIZE_MAX, 0, C_LEN_WRAP},
        {"zero len wild", NULL, NULL, 0, 0, C_OK},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int r = safe_copy(t[i].d, t[i].s, t[i].n, t[i].ov);
        printf("%-26s -> %s\n", t[i].what, cname[r]);
        if (r != t[i].want) { fprintf(stderr, "unexpected result for %s\n", t[i].what); bad++; }
    }
    /* the memmove result: a[4..12] must hold the old a[0..8] = 1..8 */
    printf("final state: a[4]=%d a[11]=%d a[12]=%d\n", a[4], a[11], a[12]);
    r_free(a);
    /* a freed block is no longer a valid target */
    printf("copy into freed a: %s\n", cname[safe_copy(a, b, 4, 0)]);
    r_free(b);
    return bad ? 1 : 0;
}
