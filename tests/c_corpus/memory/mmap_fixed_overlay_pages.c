/*
 * title: MAP_FIXED overlays inside a reserved address range
 * topic: memory
 * covers: address-space reservation, MAP_FIXED replacement, file pages spliced into anonymous region, page-table style index
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NPAGES 12

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned char page_tag(const unsigned char *p) {
    return p[0];
}

/* every byte of the page equals the tag, and the tag was never overwritten partially */
static int page_uniform(const unsigned char *p, size_t pg, unsigned char tag) {
    for (size_t i = 0; i < pg; i++)
        if (p[i] != tag)
            return 0;
    return 1;
}

int main(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    check(pg >= 4096 && (pg & (pg - 1)) == 0, "page size sane");

    /* a file with 6 pages, each filled with 0xF0 + index */
    char path[] = "mmfx_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    unsigned char *fill = malloc(pg);
    check(fill != NULL, "malloc");
    for (int i = 0; i < 6; i++) {
        memset(fill, 0xF0 + i, pg);
        check(write(fd, fill, pg) == (ssize_t)pg, "write");
    }
    free(fill);

    /* reserve a contiguous range with anonymous zero pages and tag some pages */
    unsigned char *base = mmap(NULL, pg * NPAGES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(base != MAP_FAILED, "reserve");
    for (int i = 0; i < NPAGES; i++)
        memset(base + (size_t)i * pg, 0x10 + i, pg);

    /* logical map of what each page slot should now hold: -1 anon original, >= 0 file page */
    int slot_src[NPAGES];
    unsigned char slot_tag[NPAGES];
    for (int i = 0; i < NPAGES; i++) {
        slot_src[i] = -1;
        slot_tag[i] = (unsigned char)(0x10 + i);
    }

    /* splice file pages into slots 2, 3, 7 with MAP_FIXED, in shuffled file order */
    struct {
        int slot, filepage;
    } splice[] = {{2, 5}, {3, 0}, {7, 3}};
    for (size_t k = 0; k < sizeof splice / sizeof splice[0]; k++) {
        void *want = base + (size_t)splice[k].slot * pg;
        void *got = mmap(want, pg, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, (off_t)((size_t)splice[k].filepage * pg));
        check(got == want, "MAP_FIXED lands exactly");
        slot_src[splice[k].slot] = splice[k].filepage;
        slot_tag[splice[k].slot] = (unsigned char)(0xF0 + splice[k].filepage);
    }
    printf("after splice:");
    for (int i = 0; i < NPAGES; i++)
        printf(" %02x", (unsigned)page_tag(base + (size_t)i * pg));
    printf("\n");
    for (int i = 0; i < NPAGES; i++)
        check(page_uniform(base + (size_t)i * pg, pg, slot_tag[i]), "slot content");

    /* overlay a fresh zero page over slot 3, discarding the file mapping there */
    void *z = mmap(base + 3 * pg, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    check(z == base + 3 * pg, "zero overlay");
    slot_src[3] = -2;
    slot_tag[3] = 0;
    check(page_uniform(base + 3 * pg, pg, 0), "overlay is zero");
    memset(base + 3 * pg, 0xAB, pg);
    slot_tag[3] = 0xAB;

    /* remap the same file page into two different slots: both show the same bytes */
    void *a = mmap(base + 9 * pg, pg, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, (off_t)(2 * pg));
    void *b = mmap(base + 10 * pg, pg, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, (off_t)(2 * pg));
    check(a == base + 9 * pg && b == base + 10 * pg, "double map");
    slot_src[9] = slot_src[10] = 2;
    slot_tag[9] = slot_tag[10] = 0xF2;
    check(memcmp(base + 9 * pg, base + 10 * pg, pg) == 0, "aliases equal");

    printf("after overlay:");
    for (int i = 0; i < NPAGES; i++)
        printf(" %02x", (unsigned)page_tag(base + (size_t)i * pg));
    printf("\n");
    unsigned checked = 0;
    for (int i = 0; i < NPAGES; i++) {
        check(page_uniform(base + (size_t)i * pg, pg, slot_tag[i]), "final slot content");
        checked++;
    }
    printf("slots verified: %u\n", checked);
    printf("slot origins:");
    for (int i = 0; i < NPAGES; i++) {
        if (slot_src[i] == -1)
            printf(" anon");
        else if (slot_src[i] == -2)
            printf(" zero");
        else
            printf(" file%d", slot_src[i]);
    }
    printf("\n");

    /* untouched neighbours were not disturbed by any of the fixed mappings */
    for (int i = 0; i < NPAGES; i++)
        if (slot_src[i] == -1)
            check(page_uniform(base + (size_t)i * pg, pg, (unsigned char)(0x10 + i)), "neighbour intact");

    check(munmap(base, pg * NPAGES) == 0, "munmap all");
    close(fd);
    unlink(path);
    return 0;
}
