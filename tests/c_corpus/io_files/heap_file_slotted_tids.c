/*
 * title: Heap file of slotted pages with tuple ids, forwarding and vacuum
 * topic: io_files
 * covers: slotted page layout, tuple identifiers, slot reuse, forwarding pointers on growth, page vacuum, free-space map rebuild, sequential scan
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rndn(uint32_t n) {
    uint64_t v = rnd();
    return (uint32_t)((v >> 16) % n);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static inline unsigned get16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/* Write all bytes at an offset; abort the program on failure. */
static inline void pwrite_all(int fd, const void *buf, size_t n, off_t off) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = pwrite(fd, p, n, off);
        check(w > 0, "pwrite");
        p += w;
        off += w;
        n -= (size_t)w;
    }
}

/* Read up to n bytes at offset; returns the number of bytes read (short at EOF). */
static inline size_t pread_upto(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        check(r >= 0, "pread");
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got;
}
enum { PSZ = 128, HDR = 4, SLOT = 4, MAXPAGES = 128, MAXT = 400 };
/* Page: nslots u16 | free_end u16 | slot dir (off u16, len u16) growing up from HDR | tuple data growing down from PSZ.
 * len == 0xFFFF marks a dead slot; a forwarding stub is a tuple of length 3: 0xFE page slot. */
typedef struct {
    int fd;
    int npages;
    int fsm[MAXPAGES]; /* free bytes per page (free-space map) */
    long reads, writes;
} Heap;

typedef struct { int page, slot; } Tid;

static void page_load(Heap *h, int p, unsigned char *pg) {
    check(pread_upto(h->fd, pg, PSZ, (off_t)p * PSZ) == PSZ, "page load");
    h->reads++;
}
static void page_store(Heap *h, int p, const unsigned char *pg) {
    pwrite_all(h->fd, pg, PSZ, (off_t)p * PSZ);
    h->writes++;
}
static int nslots(const unsigned char *pg) { return (int)get16(pg); }
static int free_end(const unsigned char *pg) { return (int)get16(pg + 2); }
static int slot_off(const unsigned char *pg, int s) { return (int)get16(pg + HDR + SLOT * s); }
static int slot_len(const unsigned char *pg, int s) { return (int)get16(pg + HDR + SLOT * s + 2); }
static void set_slot(unsigned char *pg, int s, int off, int len) {
    put16(pg + HDR + SLOT * s, (unsigned)off);
    put16(pg + HDR + SLOT * s + 2, (unsigned)len);
}
static int page_free(const unsigned char *pg) { return free_end(pg) - (HDR + SLOT * nslots(pg)); }

static void new_page(Heap *h) {
    unsigned char pg[PSZ];
    memset(pg, 0, PSZ);
    put16(pg, 0);
    put16(pg + 2, PSZ);
    h->fsm[h->npages] = PSZ - HDR;
    page_store(h, h->npages++, pg);
}

/* Insert into page p if it fits (reusing a dead slot when available). Returns slot or -1. */
static int page_insert(Heap *h, int p, const unsigned char *t, int len) {
    unsigned char pg[PSZ];
    page_load(h, p, pg);
    int n = nslots(pg), reuse = -1;
    for (int s = 0; s < n; s++)
        if (slot_len(pg, s) == 0xFFFF) {
            reuse = s;
            break;
        }
    int need = len + (reuse < 0 ? SLOT : 0);
    if (page_free(pg) < need)
        return -1;
    int off = free_end(pg) - len;
    memcpy(pg + off, t, (size_t)len);
    int s = reuse >= 0 ? reuse : n;
    set_slot(pg, s, off, len);
    if (reuse < 0)
        put16(pg, (unsigned)(n + 1));
    put16(pg + 2, (unsigned)off);
    h->fsm[p] = page_free(pg);
    page_store(h, p, pg);
    return s;
}

static Tid heap_insert(Heap *h, const unsigned char *t, int len) {
    for (int p = 0; p < h->npages; p++)
        if (h->fsm[p] >= len + SLOT) {
            int s = page_insert(h, p, t, len);
            if (s >= 0)
                return (Tid){p, s};
        }
    check(h->npages < MAXPAGES, "heap full");
    new_page(h);
    int s = page_insert(h, h->npages - 1, t, len);
    check(s >= 0, "fits in empty page");
    return (Tid){h->npages - 1, s};
}

/* Resolve forwarding; returns tuple length or -1 when dead. */
static int heap_get(Heap *h, Tid id, unsigned char *out) {
    for (int hop = 0; hop < 4; hop++) {
        unsigned char pg[PSZ];
        page_load(h, id.page, pg);
        check(id.slot < nslots(pg), "slot range");
        int len = slot_len(pg, id.slot);
        if (len == 0xFFFF)
            return -1;
        int off = slot_off(pg, id.slot);
        if (len == 3 && pg[off] == 0xFE) {
            id.page = pg[off + 1];
            id.slot = pg[off + 2];
            continue;
        }
        memcpy(out, pg + off, (size_t)len);
        return len;
    }
    check(0, "forward chain too long");
    return -1;
}

static void heap_delete(Heap *h, Tid id) {
    unsigned char pg[PSZ];
    page_load(h, id.page, pg);
    int len = slot_len(pg, id.slot), off = slot_off(pg, id.slot);
    if (len == 3 && pg[off] == 0xFE) {
        Tid tgt = {pg[off + 1], pg[off + 2]};
        heap_delete(h, tgt);
        page_load(h, id.page, pg);
    }
    set_slot(pg, id.slot, 0, 0xFFFF);
    page_store(h, id.page, pg);
}

/* Compact live tuples of a page toward the end; slot numbers (and so TIDs) are unchanged. */
static int vacuum_page(Heap *h, int p) {
    unsigned char pg[PSZ], np[PSZ];
    page_load(h, p, pg);
    memset(np, 0, PSZ);
    int n = nslots(pg), end = PSZ;
    put16(np, (unsigned)n);
    for (int s = 0; s < n; s++) {
        int len = slot_len(pg, s);
        if (len == 0xFFFF) {
            set_slot(np, s, 0, 0xFFFF);
            continue;
        }
        end -= len;
        memcpy(np + end, pg + slot_off(pg, s), (size_t)len);
        set_slot(np, s, end, len);
    }
    put16(np + 2, (unsigned)end);
    int gained = page_free(np) - page_free(pg);
    h->fsm[p] = page_free(np);
    page_store(h, p, np);
    return gained;
}

/* Update: in place when the new tuple fits the old slot's space, else move and leave a forwarding stub. */
static void heap_update(Heap *h, Tid id, const unsigned char *t, int len, int *moved) {
    unsigned char pg[PSZ];
    page_load(h, id.page, pg);
    int olen = slot_len(pg, id.slot), off = slot_off(pg, id.slot);
    if (olen == 3 && pg[off] == 0xFE) { /* home slot is a stub: chains stay one hop long */
        Tid tgt = {pg[off + 1], pg[off + 2]};
        unsigned char tp[PSZ];
        page_load(h, tgt.page, tp);
        int tlen = slot_len(tp, tgt.slot);
        if (len <= tlen) {
            memcpy(tp + slot_off(tp, tgt.slot), t, (size_t)len);
            set_slot(tp, tgt.slot, slot_off(tp, tgt.slot), len);
            page_store(h, tgt.page, tp);
            return;
        }
        Tid nt = heap_insert(h, t, len);
        heap_delete(h, tgt);
        page_load(h, id.page, pg);
        pg[off + 1] = (unsigned char)nt.page;
        pg[off + 2] = (unsigned char)nt.slot;
        page_store(h, id.page, pg);
        (*moved)++;
        return;
    }
    if (len <= olen) {
        memcpy(pg + off, t, (size_t)len);
        set_slot(pg, id.slot, off, len);
        page_store(h, id.page, pg);
        return;
    }
    Tid nt = heap_insert(h, t, len);
    page_load(h, id.page, pg);
    /* the stub reuses the old tuple's first 3 bytes of space */
    pg[off] = 0xFE;
    pg[off + 1] = (unsigned char)nt.page;
    pg[off + 2] = (unsigned char)nt.slot;
    set_slot(pg, id.slot, off, 3);
    page_store(h, id.page, pg);
    (*moved)++;
}

static int make_tuple(unsigned char *t, int key, int ver, int len) {
    for (int i = 0; i < len; i++)
        t[i] = (unsigned char)(key * 13 + ver * 7 + i);
    t[0] = (unsigned char)(key & 0x7F); /* never 0xFE */
    return len;
}

int main(void) {
    Heap h;
    memset(&h, 0, sizeof h);
    h.fd = open("heap.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(h.fd >= 0, "open heap");
    static Tid tid[MAXT];
    static int ver[MAXT], len_of[MAXT], alive[MAXT];
    int nkeys = 0, moved = 0, deleted = 0, updated = 0;
    for (int step = 0; step < 900; step++) {
        int op = (int)rndn(10);
        if (op < 4 || nkeys == 0) {
            if (nkeys >= MAXT)
                continue;
            unsigned char t[64];
            int len = 4 + (int)rndn(24);
            make_tuple(t, nkeys, 0, len);
            tid[nkeys] = heap_insert(&h, t, len);
            len_of[nkeys] = len;
            alive[nkeys] = 1;
            nkeys++;
        } else if (op < 7) {
            int k = (int)rndn((uint32_t)nkeys);
            if (!alive[k])
                continue;
            unsigned char t[64];
            int len = 4 + (int)rndn(32);
            ver[k]++;
            make_tuple(t, k, ver[k], len);
            heap_update(&h, tid[k], t, len, &moved);
            len_of[k] = len;
            updated++;
        } else if (op < 9) {
            int k = (int)rndn((uint32_t)nkeys);
            if (!alive[k])
                continue;
            heap_delete(&h, tid[k]);
            alive[k] = 0;
            deleted++;
        } else {
            for (int p = 0; p < h.npages; p++)
                vacuum_page(&h, p);
        }
    }
    int live = 0, forwards = 0;
    for (int k = 0; k < nkeys; k++) {
        unsigned char t[64], want[64];
        if (!alive[k])
            continue; /* a deleted key's TID may already be reused by a newer tuple */
        int len = heap_get(&h, tid[k], t);
        check(len >= 0, "live key reachable through TID");
        {
            live++;
            check(len == len_of[k], "length");
            make_tuple(want, k, ver[k], len);
            check(memcmp(t, want, (size_t)len) == 0, "tuple bytes");
        }
    }
    /* Sequential scan of raw pages counts live non-stub tuples: must equal live keys. */
    int scanned = 0, dead = 0, slots = 0;
    for (int p = 0; p < h.npages; p++) {
        unsigned char pg[PSZ];
        page_load(&h, p, pg);
        for (int s = 0; s < nslots(pg); s++) {
            slots++;
            int len = slot_len(pg, s);
            if (len == 0xFFFF)
                dead++;
            else if (len == 3 && pg[slot_off(pg, s)] == 0xFE)
                forwards++;
            else
                scanned++;
        }
    }
    check(scanned == live, "scan equals live keys");
    /* Rebuild the free-space map from disk (as after restart) and compare. */
    for (int p = 0; p < h.npages; p++) {
        unsigned char pg[PSZ];
        page_load(&h, p, pg);
        check(page_free(pg) == h.fsm[p], "free-space map rebuild");
    }
    int gained = 0;
    for (int p = 0; p < h.npages; p++)
        gained += vacuum_page(&h, p);
    for (int k = 0; k < nkeys; k++) {
        unsigned char t[64];
        check(!alive[k] || heap_get(&h, tid[k], t) >= 0, "TIDs stable across vacuum");
    }
    printf("inserted=%d updated=%d (moved with forwarding=%d) deleted=%d\n", nkeys, updated, moved, deleted);
    printf("pages=%d slots=%d live tuples=%d dead slots=%d forwarding stubs=%d\n", h.npages, slots, live, dead,
           forwards);
    printf("final vacuum reclaimed %d bytes\n", gained);
    printf("page reads=%ld writes=%ld\n", h.reads, h.writes);
    close(h.fd);
    unlink("heap.dat");
    return 0;
}
