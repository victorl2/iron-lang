/*
 * title: ELF-style string table with suffix sharing
 * topic: memory
 * covers: string table layout, tail merging, reverse sort, offset lookup, deduplication
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 64

typedef struct {
    const char *s;
    size_t len;
    uint32_t off;   /* assigned offset in the table */
    int orig;       /* original position */
    int dup_of;     /* index in sorted array of the identical string, or -1 */
} Ent;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* compare strings by their reversed bytes, so that suffix relatives sort adjacently */
static int rev_cmp(const char *a, size_t la, const char *b, size_t lb) {
    size_t i = 0;
    while (i < la && i < lb) {
        unsigned char ca = (unsigned char)a[la - 1 - i], cb = (unsigned char)b[lb - 1 - i];
        if (ca != cb)
            return ca < cb ? -1 : 1;
        i++;
    }
    if (la != lb)
        return la < lb ? -1 : 1;
    return 0;
}

static int cmp_ent(const void *pa, const void *pb) {
    const Ent *a = pa, *b = pb;
    int c = rev_cmp(a->s, a->len, b->s, b->len);
    if (c)
        return c;
    return a->orig - b->orig;
}

static int is_suffix(const char *suf, size_t ls, const char *s, size_t l) {
    return ls <= l && memcmp(s + l - ls, suf, ls) == 0;
}

/* Build the table; returns its length. Offsets point at the first byte of each name. */
static size_t build(const char *const *names, int n, unsigned char *tab, uint32_t *offs_by_orig, int *shared_out) {
    Ent e[MAXN];
    for (int i = 0; i < n; i++) {
        e[i].s = names[i];
        e[i].len = strlen(names[i]);
        e[i].orig = i;
        e[i].dup_of = -1;
    }
    qsort(e, (size_t)n, sizeof e[0], cmp_ent);
    /* after the reverse sort, a string that is a suffix of another sorts right before
       something that ends with it; process from the longest end backwards */
    size_t pos = 1; /* offset 0 is the empty string, as in ELF */
    tab[0] = 0;
    int shared = 0;
    /* walk from the end: the last entry in reverse order holds every suffix relative before it */
    int i = n - 1;
    while (i >= 0) {
        /* e[i] is a "root": nothing later ends with it (checked by the sweep) */
        uint32_t root_off = (uint32_t)pos;
        memcpy(tab + pos, e[i].s, e[i].len + 1);
        pos += e[i].len + 1;
        e[i].off = root_off;
        int j = i - 1;
        while (j >= 0 && is_suffix(e[j].s, e[j].len, e[i].s, e[i].len)) {
            e[j].off = root_off + (uint32_t)(e[i].len - e[j].len);
            shared++;
            j--;
        }
        i = j;
    }
    for (int k = 0; k < n; k++)
        offs_by_orig[e[k].orig] = e[k].off;
    *shared_out = shared;
    return pos;
}

static size_t naive_size(const char *const *names, int n) {
    size_t t = 1;
    for (int i = 0; i < n; i++)
        t += strlen(names[i]) + 1;
    return t;
}

int main(void) {
    static const char *const sym[] = {
        "main",     "init",     "sinit",      "_init",     "reinit",     "printf",   "fprintf",  "vfprintf",
        "sprintf",  "memcpy",   "copy",       "strcpy",    "strncpy",    "open",     "fopen",    "reopen",
        "close",    "fclose",   "read",       "fread",     "thread",     "pthread",  "write",    "fwrite",
        "a",        "ab",       "b",          "ab",        "cab",        "init",     "",         "x.debug",
        ".debug",   "debug",    ".text",      "text",      ".rodata",    ".data",    ".bss",     "bss",
    };
    int n = (int)(sizeof sym / sizeof sym[0]);
    unsigned char tab[1024];
    uint32_t offs[MAXN];
    int shared = 0;
    size_t len = build(sym, n, tab, offs, &shared);
    size_t naive = naive_size(sym, n);
    printf("names=%d table=%zu naive=%zu saved=%zu shared=%d\n", n, len, naive, naive - len, shared);
    check(len <= naive, "never larger than naive");

    /* every original name must be recoverable from its offset */
    for (int i = 0; i < n; i++) {
        check(offs[i] < len, "offset in range");
        check(strcmp((const char *)tab + offs[i], sym[i]) == 0, "lookup by offset");
    }
    printf("all %d lookups verified\n", n);

    /* identical strings share one offset */
    int same = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(sym[i], sym[j]) == 0) {
                check(offs[i] == offs[j], "duplicates share an offset");
                same++;
            }
    printf("duplicate pairs sharing storage: %d\n", same);

    /* sample table dump: offsets of a few names */
    const int probe[] = {0, 1, 3, 5, 9, 13, 17, 24, 25, 30, 33};
    for (size_t k = 0; k < sizeof probe / sizeof probe[0]; k++)
        printf("  %-8s -> offset %3u\n", sym[probe[k]][0] ? sym[probe[k]] : "(empty)", (unsigned)offs[probe[k]]);

    /* count distinct table strings by scanning for NUL terminators */
    int roots = 0;
    for (size_t p = 1; p < len; p++)
        if (tab[p] == 0)
            roots++;
    printf("stored strings: %d\n", roots);
    check(roots == n - shared, "roots plus shared equals names");

    /* a second table where nothing overlaps: should equal the naive size minus duplicates */
    const char *const flat[] = {"alpha", "beta", "gamma", "delta"};
    int sh2 = 0;
    uint32_t o2[4];
    size_t l2 = build(flat, 4, tab, o2, &sh2);
    printf("disjoint set: table=%zu naive=%zu shared=%d\n", l2, naive_size(flat, 4), sh2);
    check(l2 == naive_size(flat, 4) && sh2 == 0, "no sharing");
    /* "beta" and "delta" both end in 'a', but neither is a suffix of the other */
    return 0;
}
