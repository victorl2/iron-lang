/*
 * title: Flexible array member records in a variable-size heap
 * topic: memory
 * covers: flexible array member, sizeof plus offsetof allocation, variable-length records, packed record stream
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t id;
    uint16_t count;
    uint16_t cap;
    int32_t items[]; /* flexible array member */
} IntList;

typedef struct {
    uint16_t len;
    uint8_t kind;
    char text[]; /* bytes, not NUL terminated on the wire */
} Str;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* allocation size: offsetof avoids counting trailing padding twice */
static size_t list_bytes(size_t cap) {
    return offsetof(IntList, items) + cap * sizeof(int32_t);
}

static IntList *list_new(uint32_t id, uint16_t cap) {
    IntList *l = malloc(list_bytes(cap));
    if (!l)
        exit(2);
    l->id = id;
    l->count = 0;
    l->cap = cap;
    return l;
}

/* growing needs a realloc that returns the new pointer: the address of the struct changes */
static IntList *list_push(IntList *l, int32_t v) {
    if (l->count == l->cap) {
        uint16_t ncap = (uint16_t)(l->cap ? l->cap * 2 : 2);
        IntList *n = realloc(l, list_bytes(ncap));
        if (!n)
            exit(2);
        n->cap = ncap;
        l = n;
    }
    l->items[l->count++] = v;
    return l;
}

/* variable-length records in one contiguous arena, each aligned to 4 bytes */
typedef struct {
    unsigned char *buf;
    size_t used, cap;
} Pack;

static Str *pack_str(Pack *p, uint8_t kind, const char *s, size_t n) {
    size_t at = (p->used + 3) & ~(size_t)3;
    size_t need = offsetof(Str, text) + n;
    if (at + need > p->cap)
        return NULL;
    Str *r = (Str *)(void *)(p->buf + at);
    r->len = (uint16_t)n;
    r->kind = kind;
    memcpy(r->text, s, n);
    p->used = at + need;
    return r;
}

static const Str *pack_next(const Pack *p, const Str *cur) {
    size_t at;
    if (!cur)
        at = 0;
    else
        at = (size_t)((const unsigned char *)cur - p->buf) + offsetof(Str, text) + cur->len;
    at = (at + 3) & ~(size_t)3;
    if (at >= p->used)
        return NULL;
    return (const Str *)(const void *)(p->buf + at);
}

int main(void) {
    printf("sizeof(IntList)=%zu offsetof(items)=%zu\n", sizeof(IntList), offsetof(IntList, items));
    printf("sizeof(Str)=%zu offsetof(text)=%zu\n", sizeof(Str), offsetof(Str, text));
    check(list_bytes(0) == sizeof(IntList), "empty list is header only");
    check(list_bytes(3) == 8 + 12, "bytes for 3 items");

    IntList *l = list_new(7, 0);
    int reallocs = 0;
    uint16_t last_cap = l->cap;
    for (int i = 0; i < 100; i++) {
        l = list_push(l, i * i - 50);
        if (l->cap != last_cap) {
            reallocs++;
            last_cap = l->cap;
        }
    }
    long sum = 0;
    for (int i = 0; i < l->count; i++)
        sum += l->items[i];
    printf("list id=%u count=%u cap=%u grows=%d sum=%ld\n", (unsigned)l->id, (unsigned)l->count, (unsigned)l->cap,
           reallocs, sum);
    long expect = 0;
    for (int i = 0; i < 100; i++)
        expect += i * i - 50;
    check(sum == expect, "sum");

    /* copying a flexible-array struct needs an explicit size; plain assignment copies only the header */
    IntList *copy = list_new(l->id, l->count);
    memcpy(copy, l, list_bytes(l->count));
    check(copy->count == l->count && copy->items[99] == l->items[99], "deep memcpy");
    IntList head_only = *l;
    (void)head_only; /* only id/count/cap are copied by assignment */
    printf("copy last item: %d\n", (int)copy->items[copy->count - 1]);
    free(copy);
    free(l);

    /* stream of variable-size string records */
    unsigned char storage[512];
    Pack p = {storage, 0, sizeof storage};
    static const char *words[] = {"alpha", "b", "gamma-ray", "", "delta force", "e", "ordinal", "zeta"};
    int stored = 0;
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        Str *s = pack_str(&p, (uint8_t)(i % 3), words[i], strlen(words[i]));
        check(s != NULL, "pack");
        stored++;
    }
    printf("packed %d records into %zu bytes\n", stored, p.used);
    int seen = 0;
    size_t textbytes = 0;
    for (const Str *s = pack_next(&p, NULL); s; s = pack_next(&p, s)) {
        printf("  kind=%u len=%2u '%.*s'\n", (unsigned)s->kind, (unsigned)s->len, (int)s->len, s->text);
        check(s->len == strlen(words[seen]) && memcmp(s->text, words[seen], s->len) == 0, "record content");
        textbytes += s->len;
        seen++;
    }
    check(seen == stored, "iterated all");
    printf("text bytes=%zu overhead=%zu\n", textbytes, p.used - textbytes);

    /* refuses when full */
    Pack tiny = {storage, 0, 10};
    check(pack_str(&tiny, 1, "toolongstring", 13) == NULL, "overflow refused");
    printf("overflow refused\n");
    return 0;
}
