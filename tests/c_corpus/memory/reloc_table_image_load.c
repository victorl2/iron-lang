/*
 * title: Relocatable image with a relocation table applied at load time
 * topic: memory
 * covers: link-time base address, absolute pointer fields, relocation records, load-time delta fixup, loading at two addresses
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The image is built as if it will live at LINK_BASE. Pointer fields hold LINK_BASE + offset.
 * The relocation table lists the image offsets of every such field. Loading at another address
 * adds (actual_base - LINK_BASE) to each listed field.
 */
#define LINK_BASE 0x40000000ull
#define IMG_CAP 4096u

typedef struct Entry {
    uint64_t name;  /* pointer to NUL terminated string */
    uint64_t next;  /* pointer to next Entry or 0 */
    uint32_t value;
    uint32_t weight;
} Entry;

typedef struct {
    uint64_t first;        /* pointer to first Entry */
    uint64_t table[6];     /* pointers to individual entries: a jump-table like array */
    uint32_t count;
    uint32_t reloc_count;
} Root;

typedef struct {
    unsigned char bytes[IMG_CAP];
    uint32_t used;
    uint32_t relocs[64]; /* offsets of pointer fields */
    uint32_t nreloc;
} Image;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t img_take(Image *im, size_t n) {
    uint32_t off = (im->used + 7u) & ~7u;
    check(off + n <= IMG_CAP, "image capacity");
    im->used = off + (uint32_t)n;
    return off;
}

static void img_ptr_field(Image *im, uint32_t field_off, uint32_t target_off) {
    uint64_t v = target_off ? LINK_BASE + target_off : 0;
    memcpy(im->bytes + field_off, &v, sizeof v);
    if (target_off) {
        check(im->nreloc < 64, "reloc capacity");
        im->relocs[im->nreloc++] = field_off;
    }
}

static uint32_t build(Image *im) {
    memset(im, 0, sizeof *im);
    uint32_t root = img_take(im, sizeof(Root));
    static const char *names[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta"};
    uint32_t ent[6];
    uint32_t nm[6];
    for (int i = 0; i < 6; i++) {
        size_t l = strlen(names[i]) + 1;
        nm[i] = img_take(im, l);
        memcpy(im->bytes + nm[i], names[i], l);
        ent[i] = img_take(im, sizeof(Entry));
    }
    for (int i = 0; i < 6; i++) {
        uint32_t value = (uint32_t)(i * i + 3), weight = (uint32_t)(10 - i);
        memcpy(im->bytes + ent[i] + offsetof(Entry, value), &value, 4);
        memcpy(im->bytes + ent[i] + offsetof(Entry, weight), &weight, 4);
        img_ptr_field(im, ent[i] + (uint32_t)offsetof(Entry, name), nm[i]);
        img_ptr_field(im, ent[i] + (uint32_t)offsetof(Entry, next), i + 1 < 6 ? ent[i + 1] : 0);
    }
    img_ptr_field(im, root + (uint32_t)offsetof(Root, first), ent[0]);
    for (int i = 0; i < 6; i++)
        img_ptr_field(im, root + (uint32_t)offsetof(Root, table) + (uint32_t)i * 8u, ent[5 - i]);
    uint32_t cnt = 6, rc = im->nreloc;
    memcpy(im->bytes + root + offsetof(Root, count), &cnt, 4);
    memcpy(im->bytes + root + offsetof(Root, reloc_count), &rc, 4);
    return root;
}

/* load: copy to `dst` (8-byte aligned) and relocate for that address */
static void load(const Image *im, unsigned char *dst) {
    memcpy(dst, im->bytes, im->used);
    uint64_t delta = (uint64_t)(uintptr_t)dst - LINK_BASE; /* modular arithmetic, wraps by design */
    for (uint32_t i = 0; i < im->nreloc; i++) {
        uint64_t v;
        memcpy(&v, dst + im->relocs[i], sizeof v);
        v += delta;
        memcpy(dst + im->relocs[i], &v, sizeof v);
    }
}

static const void *as_ptr(uint64_t v) {
    return (const void *)(uintptr_t)v;
}

typedef struct {
    unsigned count;
    unsigned long value_sum, weighted, name_bytes;
    char order[64];
} Walk;

static Walk walk(const unsigned char *base, uint32_t root_off) {
    const Root *r = (const Root *)(const void *)(base + root_off);
    Walk w;
    memset(&w, 0, sizeof w);
    for (const Entry *e = as_ptr(r->first); e; e = as_ptr(e->next)) {
        const char *name = as_ptr(e->name);
        w.count++;
        w.value_sum += e->value;
        w.weighted += (unsigned long)e->value * e->weight;
        w.name_bytes += strlen(name);
        size_t at = strlen(w.order);
        snprintf(w.order + at, sizeof w.order - at, "%c", name[0]);
    }
    /* the table lists entries in reverse; its first name must be the last list entry's */
    const Entry *t0 = as_ptr(r->table[0]);
    const char *n0 = as_ptr(t0->name);
    check(strcmp(n0, "zeta") == 0, "table[0] is zeta");
    check(r->count == w.count, "count field");
    return w;
}

int main(void) {
    Image im;
    uint32_t root = build(&im);
    printf("image bytes used: %u, relocations: %u\n", (unsigned)im.used, (unsigned)im.nreloc);

    /* before relocation the pointers are meaningless: they hold link-time values */
    uint64_t raw;
    memcpy(&raw, im.bytes + root + offsetof(Root, first), 8);
    printf("link-time first pointer minus base: %llu\n", (unsigned long long)(raw - LINK_BASE));

    /* load twice at different addresses, including a misaligned-by-8 offset within a bigger buffer */
    unsigned char *a = malloc(IMG_CAP);
    unsigned char *big = malloc(IMG_CAP + 128);
    check(a && big, "malloc");
    unsigned char *b = big + 64;
    load(&im, a);
    load(&im, b);
    Walk wa = walk(a, root), wb = walk(b, root);
    printf("copy A: entries=%u value-sum=%lu weighted=%lu name-bytes=%lu order=%s\n", wa.count, wa.value_sum, wa.weighted,
           wa.name_bytes, wa.order);
    printf("copy B: entries=%u value-sum=%lu weighted=%lu name-bytes=%lu order=%s\n", wb.count, wb.value_sum, wb.weighted,
           wb.name_bytes, wb.order);
    check(wa.count == wb.count && wa.value_sum == wb.value_sum && wa.weighted == wb.weighted &&
              wa.name_bytes == wb.name_bytes && strcmp(wa.order, wb.order) == 0,
          "both loads behave identically");

    /* the relocated pointers stay inside their own copy */
    const Root *ra = (const Root *)(const void *)(a + root);
    const Root *rb = (const Root *)(const void *)(b + root);
    check((const unsigned char *)as_ptr(ra->first) >= a && (const unsigned char *)as_ptr(ra->first) < a + im.used, "A inside A");
    check((const unsigned char *)as_ptr(rb->first) >= b && (const unsigned char *)as_ptr(rb->first) < b + im.used, "B inside B");
    check(as_ptr(ra->first) != as_ptr(rb->first), "copies are independent");

    /* mutating A does not disturb B */
    Entry *e0 = (Entry *)(void *)(a + (uint32_t)((uintptr_t)as_ptr(ra->first) - (uintptr_t)a));
    e0->value = 1000;
    Walk wa2 = walk(a, root), wb2 = walk(b, root);
    printf("after edit A value-sum=%lu, B value-sum=%lu\n", wa2.value_sum, wb2.value_sum);
    check(wa2.value_sum == wa.value_sum - 3 + 1000 && wb2.value_sum == wb.value_sum, "edit isolated");

    /* skipping relocation yields wrong (link-time) pointers: demonstrate on offsets only */
    uint64_t unrelocated_delta = LINK_BASE - LINK_BASE;
    check(unrelocated_delta == 0, "sanity");
    unsigned unfixed = 0;
    for (uint32_t i = 0; i < im.nreloc; i++) {
        uint64_t v;
        memcpy(&v, im.bytes + im.relocs[i], 8);
        if (v >= LINK_BASE && v < LINK_BASE + im.used)
            unfixed++;
    }
    printf("pointer fields holding link-time addresses in the raw image: %u of %u\n", unfixed, (unsigned)im.nreloc);
    check(unfixed == im.nreloc, "raw image is all link-time");

    free(a);
    free(big);
    return 0;
}
