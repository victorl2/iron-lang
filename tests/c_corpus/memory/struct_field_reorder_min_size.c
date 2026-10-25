/*
 * title: Layout simulator and optimal field ordering
 * topic: memory
 * covers: struct layout algorithm, permutation search, alignment sort heuristic, sizeof cross-check
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    unsigned size;
    unsigned align;
} Field;

typedef struct {
    unsigned size;
    unsigned align;
    unsigned pad;
} Layout;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Layout layout_of(const Field *f, const int *order, int n) {
    Layout l = {0, 1, 0};
    unsigned off = 0;
    for (int i = 0; i < n; i++) {
        const Field *fi = &f[order[i]];
        unsigned aligned = (off + fi->align - 1) / fi->align * fi->align;
        l.pad += aligned - off;
        off = aligned + fi->size;
        if (fi->align > l.align)
            l.align = fi->align;
    }
    unsigned total = (off + l.align - 1) / l.align * l.align;
    l.pad += total - off;
    l.size = total;
    return l;
}

/* enumerate all permutations (Heap's algorithm), tracking best and worst */
static void permute(const Field *f, int *order, int k, int n, Layout *best, Layout *worst, int *best_order,
                    unsigned long *count) {
    if (k == 1) {
        Layout l = layout_of(f, order, n);
        (*count)++;
        if (l.size < best->size) {
            *best = l;
            memcpy(best_order, order, sizeof(int) * (size_t)n);
        }
        if (l.size > worst->size)
            *worst = l;
        return;
    }
    for (int i = 0; i < k; i++) {
        permute(f, order, k - 1, n, best, worst, best_order, count);
        int j = (k % 2 == 0) ? i : 0;
        int t = order[j];
        order[j] = order[k - 1];
        order[k - 1] = t;
    }
}

static const Field *g_fields;

static int cmp_align_desc(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    if (g_fields[a].align != g_fields[b].align)
        return g_fields[a].align > g_fields[b].align ? -1 : 1;
    if (g_fields[a].size != g_fields[b].size)
        return g_fields[a].size > g_fields[b].size ? -1 : 1;
    return a - b;
}

static void analyze(const char *title, const Field *f, int n) {
    int order[8], best_order[8], sorted[8];
    for (int i = 0; i < n; i++)
        order[i] = sorted[i] = i;
    Layout declared = layout_of(f, order, n);
    Layout best = {1000000, 1, 0}, worst = {0, 1, 0};
    unsigned long count = 0;
    permute(f, order, n, n, &best, &worst, best_order, &count);
    g_fields = f;
    qsort(sorted, (size_t)n, sizeof(int), cmp_align_desc);
    Layout heur = layout_of(f, sorted, n);
    printf("%s (%d fields, %lu orderings)\n", title, n, count);
    printf("  declared order: size %u pad %u\n", declared.size, declared.pad);
    printf("  worst order:    size %u pad %u\n", worst.size, worst.pad);
    printf("  best order:     size %u pad %u\n", best.size, best.pad);
    printf("  align-desc sort: size %u pad %u\n", heur.size, heur.pad);
    check(heur.size == best.size, "alignment-descending sort is optimal");
    check(best.size <= declared.size && declared.size <= worst.size, "ordering bounds");
    unsigned payload = 0;
    for (int i = 0; i < n; i++)
        payload += f[i].size;
    check(best.size >= payload, "size covers payload");
    printf("  payload %u, best waste %u\n", payload, best.size - payload);
}

typedef struct {
    uint8_t a;
    uint64_t b;
    uint8_t c;
    uint32_t d;
    uint16_t e;
} RealMixed;

typedef struct {
    uint64_t b;
    uint32_t d;
    uint16_t e;
    uint8_t a;
    uint8_t c;
} RealSorted;

int main(void) {
    const Field f1[] = {{"a", 1, 1}, {"b", 8, 8}, {"c", 1, 1}, {"d", 4, 4}, {"e", 2, 2}};
    analyze("mixed5", f1, 5);
    /* the simulator agrees with the real compiler */
    int id[5] = {0, 1, 2, 3, 4};
    check(layout_of(f1, id, 5).size == sizeof(RealMixed), "simulator vs sizeof (declared)");
    int srt[5] = {1, 3, 4, 0, 2};
    check(layout_of(f1, srt, 5).size == sizeof(RealSorted), "simulator vs sizeof (sorted)");
    printf("simulator matches compiler: %zu and %zu\n", sizeof(RealMixed), sizeof(RealSorted));

    const Field f2[] = {{"flag", 1, 1}, {"id", 4, 4}, {"ptr", 8, 8}, {"len", 2, 2}, {"tag", 1, 1}, {"ts", 8, 8}};
    analyze("record6", f2, 6);

    const Field f3[] = {{"b1", 1, 1}, {"w1", 2, 2}, {"b2", 1, 1}, {"w2", 2, 2}, {"q", 8, 8}, {"b3", 1, 1}, {"d", 4, 4}};
    analyze("wide7", f3, 7);

    /* arrays and odd sizes: a 6-byte array has alignment 2, a 5-byte one alignment 1 */
    const Field f4[] = {{"mac", 6, 1}, {"vlan", 2, 2}, {"ttl", 1, 1}, {"addr", 4, 4}, {"name5", 5, 1}};
    analyze("odd5", f4, 5);
    return 0;
}
