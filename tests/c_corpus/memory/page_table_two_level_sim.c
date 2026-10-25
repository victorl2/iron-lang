/*
 * title: Two-level page table and TLB simulation
 * topic: memory
 * covers: virtual address decomposition, page directory/table walk, permission faults, dirty/accessed bits, direct-mapped TLB
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 32-bit virtual addresses: 10 bit directory, 10 bit table, 12 bit offset (a simulated MMU, not the host's) */
#define OFFSET_BITS 12u
#define TABLE_BITS 10u
#define PAGE_BYTES (1u << OFFSET_BITS)
#define NFRAMES 64u

enum { P_PRESENT = 1, P_WRITE = 2, P_USER = 4, P_ACCESSED = 8, P_DIRTY = 16 };

typedef struct {
    uint32_t frame_and_flags; /* frame number << 12 | flags */
} Pte;

typedef struct {
    Pte *tables[1u << TABLE_BITS]; /* page directory: pointers to second-level tables */
} Mmu;

typedef enum { T_OK, T_NOT_PRESENT, T_PROTECTION } Trans;

typedef struct {
    uint32_t vpn;
    uint32_t pte_frame_flags;
    int valid;
} TlbEnt;

typedef struct {
    TlbEnt e[8];
    unsigned long hits, misses, flushes;
} Tlb;

static unsigned char phys[NFRAMES][PAGE_BYTES];
static unsigned char frame_used[NFRAMES];
static unsigned tables_allocated;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int frame_alloc(void) {
    for (unsigned i = 0; i < NFRAMES; i++)
        if (!frame_used[i]) {
            frame_used[i] = 1;
            memset(phys[i], 0, PAGE_BYTES);
            return (int)i;
        }
    return -1;
}

static Pte *walk(Mmu *m, uint32_t va, int create) {
    uint32_t di = va >> (OFFSET_BITS + TABLE_BITS);
    uint32_t ti = (va >> OFFSET_BITS) & ((1u << TABLE_BITS) - 1);
    if (!m->tables[di]) {
        if (!create)
            return NULL;
        m->tables[di] = calloc(1u << TABLE_BITS, sizeof(Pte));
        check(m->tables[di] != NULL, "table alloc");
        tables_allocated++;
    }
    return &m->tables[di][ti];
}

static int map_page(Mmu *m, uint32_t va, uint32_t flags) {
    Pte *p = walk(m, va, 1);
    if (p->frame_and_flags & P_PRESENT)
        return -1;
    int f = frame_alloc();
    if (f < 0)
        return -2;
    p->frame_and_flags = ((uint32_t)f << OFFSET_BITS) | flags | P_PRESENT;
    return f;
}

static void unmap_page(Mmu *m, uint32_t va, Tlb *tlb) {
    Pte *p = walk(m, va, 0);
    check(p && (p->frame_and_flags & P_PRESENT), "unmap present");
    frame_used[p->frame_and_flags >> OFFSET_BITS] = 0;
    p->frame_and_flags = 0;
    for (unsigned i = 0; i < 8; i++)
        tlb->e[i].valid = 0; /* shootdown */
    tlb->flushes++;
}

static Trans translate(Mmu *m, Tlb *tlb, uint32_t va, int write, uint32_t *pa) {
    uint32_t vpn = va >> OFFSET_BITS;
    TlbEnt *t = &tlb->e[vpn & 7u];
    Pte *pte;
    if (t->valid && t->vpn == vpn) {
        tlb->hits++;
        pte = NULL;
    } else {
        tlb->misses++;
        pte = walk(m, va, 0);
        if (!pte || !(pte->frame_and_flags & P_PRESENT))
            return T_NOT_PRESENT;
        t->vpn = vpn;
        t->valid = 1;
    }
    /* the authoritative entry is always re-read from the table for flag updates */
    if (!pte)
        pte = walk(m, va, 0);
    if (write && !(pte->frame_and_flags & P_WRITE))
        return T_PROTECTION;
    pte->frame_and_flags |= P_ACCESSED | (write ? P_DIRTY : 0);
    *pa = (pte->frame_and_flags & ~(PAGE_BYTES - 1)) | (va & (PAGE_BYTES - 1));
    return T_OK;
}

static int vwrite(Mmu *m, Tlb *tlb, uint32_t va, unsigned char v, Trans *why) {
    uint32_t pa;
    *why = translate(m, tlb, va, 1, &pa);
    if (*why != T_OK)
        return -1;
    phys[pa >> OFFSET_BITS][pa & (PAGE_BYTES - 1)] = v;
    return 0;
}

static int vread(Mmu *m, Tlb *tlb, uint32_t va, unsigned char *v, Trans *why) {
    uint32_t pa;
    *why = translate(m, tlb, va, 0, &pa);
    if (*why != T_OK)
        return -1;
    *v = phys[pa >> OFFSET_BITS][pa & (PAGE_BYTES - 1)];
    return 0;
}

int main(void) {
    static Mmu mmu;
    Tlb tlb;
    memset(&tlb, 0, sizeof tlb);

    /* three regions far apart: code (read only), heap (rw), stack (rw, high) */
    struct {
        const char *name;
        uint32_t base;
        unsigned npages;
        uint32_t flags;
    } regions[] = {
        {"code", 0x00400000u, 6, P_USER},
        {"heap", 0x10000000u, 20, P_USER | P_WRITE},
        {"stack", 0xBFFFC000u, 4, P_USER | P_WRITE},
    };
    for (size_t r = 0; r < 3; r++)
        for (unsigned i = 0; i < regions[r].npages; i++)
            check(map_page(&mmu, regions[r].base + i * PAGE_BYTES, regions[r].flags) >= 0, "map");
    printf("second-level tables allocated: %u\n", tables_allocated);
    unsigned used = 0;
    for (unsigned i = 0; i < NFRAMES; i++)
        used += frame_used[i];
    printf("frames in use: %u of %u\n", used, NFRAMES);

    /* decomposition of a few addresses */
    uint32_t sample[] = {0x00400ABCu, 0x10013FFFu, 0xBFFFF123u};
    for (size_t i = 0; i < 3; i++)
        printf("va 0x%08x -> dir %u table %u offset 0x%03x\n", (unsigned)sample[i],
               (unsigned)(sample[i] >> 22), (unsigned)((sample[i] >> 12) & 1023u), (unsigned)(sample[i] & 4095u));

    /* write a pattern through virtual addresses across the heap and read it back */
    Trans why;
    unsigned char v;
    uint32_t heap = regions[1].base;
    for (uint32_t i = 0; i < 20 * PAGE_BYTES; i += 97)
        check(vwrite(&mmu, &tlb, heap + i, (unsigned char)(i / 97), &why) == 0, "heap write");
    unsigned long sum = 0;
    for (uint32_t i = 0; i < 20 * PAGE_BYTES; i += 97) {
        check(vread(&mmu, &tlb, heap + i, &v, &why) == 0 && v == (unsigned char)(i / 97), "heap read");
        sum += v;
    }
    printf("heap pattern checksum: %lu\n", sum);

    /* faults */
    int f1 = vwrite(&mmu, &tlb, regions[0].base + 8, 1, &why);
    printf("write to code: %s\n", f1 < 0 && why == T_PROTECTION ? "protection fault" : "unexpected");
    check(f1 < 0 && why == T_PROTECTION, "code is read only");
    int f2 = vread(&mmu, &tlb, 0x20000000u, &v, &why);
    printf("read unmapped: %s\n", f2 < 0 && why == T_NOT_PRESENT ? "not-present fault" : "unexpected");
    check(f2 < 0 && why == T_NOT_PRESENT, "unmapped");
    int f3 = vread(&mmu, &tlb, regions[0].base + 6 * PAGE_BYTES, &v, &why);
    check(f3 < 0 && why == T_NOT_PRESENT, "one past the code region");
    check(vread(&mmu, &tlb, regions[0].base + 6 * PAGE_BYTES - 1, &v, &why) == 0 && v == 0, "last code byte");
    printf("region edges honoured\n");

    /* accessed and dirty bits */
    unsigned accessed = 0, dirty = 0;
    for (size_t r = 0; r < 3; r++)
        for (unsigned i = 0; i < regions[r].npages; i++) {
            Pte *p = walk(&mmu, regions[r].base + i * PAGE_BYTES, 0);
            accessed += (p->frame_and_flags & P_ACCESSED) != 0;
            dirty += (p->frame_and_flags & P_DIRTY) != 0;
        }
    printf("accessed pages: %u dirty pages: %u\n", accessed, dirty);
    check(dirty == 20, "only heap pages dirty");

    /* unmap half the heap, freeing frames, then remap them into different frames */
    for (unsigned i = 0; i < 20; i += 2)
        unmap_page(&mmu, heap + i * PAGE_BYTES, &tlb);
    used = 0;
    for (unsigned i = 0; i < NFRAMES; i++)
        used += frame_used[i];
    printf("frames after unmapping 10 heap pages: %u\n", used);
    check(vread(&mmu, &tlb, heap, &v, &why) < 0 && why == T_NOT_PRESENT, "unmapped heap page faults");
    check(vread(&mmu, &tlb, heap + PAGE_BYTES, &v, &why) == 0, "odd pages survive");
    for (unsigned i = 0; i < 20; i += 2)
        check(map_page(&mmu, heap + i * PAGE_BYTES, P_USER | P_WRITE) >= 0, "remap");
    check(vread(&mmu, &tlb, heap, &v, &why) == 0 && v == 0, "remapped page is zeroed");

    /* exhaust physical memory */
    unsigned extra = 0;
    while (map_page(&mmu, 0x30000000u + extra * PAGE_BYTES, P_USER) >= 0)
        extra++;
    printf("extra pages mapped before frames ran out: %u\n", extra);
    check(map_page(&mmu, 0x30000000u + extra * PAGE_BYTES, P_USER) == -2, "out of frames");

    printf("tlb: hits=%lu misses=%lu flushes=%lu\n", tlb.hits, tlb.misses, tlb.flushes);
    for (unsigned d = 0; d < (1u << TABLE_BITS); d++)
        free(mmu.tables[d]);
    return 0;
}
