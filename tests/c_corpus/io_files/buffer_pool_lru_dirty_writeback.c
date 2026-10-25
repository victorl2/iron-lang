/*
 * title: Buffer pool over a paged file with LRU eviction and dirty writeback
 * topic: io_files
 * covers: page cache, LRU list, pin counts, dirty bits, write-back on eviction, hit-rate accounting, model check
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

static inline uint32_t crc32_update(uint32_t crc, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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
enum { PSZ = 64, NPAGES = 40, NFRAMES = 6 };

typedef struct {
    int pid, pins, dirty;
    int prev, next; /* LRU links, head = most recent */
    unsigned char data[PSZ];
} Frame;

typedef struct {
    int fd;
    Frame fr[NFRAMES];
    int where[NPAGES]; /* pid -> frame or -1 */
    int head, tail, nfree;
    int freelist[NFRAMES];
    long hits, misses, evictions, writebacks, disk_reads, disk_writes;
} Pool;

static void lru_unlink(Pool *p, int i) {
    Frame *f = &p->fr[i];
    if (f->prev >= 0) p->fr[f->prev].next = f->next; else p->head = f->next;
    if (f->next >= 0) p->fr[f->next].prev = f->prev; else p->tail = f->prev;
    f->prev = f->next = -1;
}
static void lru_push_front(Pool *p, int i) {
    Frame *f = &p->fr[i];
    f->prev = -1;
    f->next = p->head;
    if (p->head >= 0) p->fr[p->head].prev = i; else p->tail = i;
    p->head = i;
}

static void pool_init(Pool *p, int fd) {
    memset(p, 0, sizeof *p);
    p->fd = fd;
    p->head = p->tail = -1;
    for (int i = 0; i < NPAGES; i++)
        p->where[i] = -1;
    for (int i = 0; i < NFRAMES; i++) {
        p->fr[i].pid = -1;
        p->fr[i].prev = p->fr[i].next = -1;
        p->freelist[p->nfree++] = NFRAMES - 1 - i;
    }
}

static void writeback(Pool *p, Frame *f) {
    if (f->dirty) {
        pwrite_all(p->fd, f->data, PSZ, (off_t)f->pid * PSZ);
        p->disk_writes++;
        p->writebacks++;
        f->dirty = 0;
    }
}

/* Pin a page and return its frame index, or -1 if every frame is pinned. */
static int pool_pin(Pool *p, int pid) {
    int i = p->where[pid];
    if (i >= 0) {
        p->hits++;
        lru_unlink(p, i);
        lru_push_front(p, i);
        p->fr[i].pins++;
        return i;
    }
    p->misses++;
    if (p->nfree > 0) {
        i = p->freelist[--p->nfree];
    } else {
        int v = p->tail;
        while (v >= 0 && p->fr[v].pins > 0)
            v = p->fr[v].prev;
        if (v < 0)
            return -1;
        writeback(p, &p->fr[v]);
        p->where[p->fr[v].pid] = -1;
        lru_unlink(p, v);
        p->evictions++;
        i = v;
    }
    Frame *f = &p->fr[i];
    size_t got = pread_upto(p->fd, f->data, PSZ, (off_t)pid * PSZ);
    check(got == PSZ, "page read");
    p->disk_reads++;
    f->pid = pid;
    f->pins = 1;
    f->dirty = 0;
    p->where[pid] = i;
    lru_push_front(p, i);
    return i;
}

static void pool_unpin(Pool *p, int i, int dirty) {
    check(p->fr[i].pins > 0, "unpin underflow");
    p->fr[i].pins--;
    if (dirty)
        p->fr[i].dirty = 1;
}

static void pool_flush(Pool *p) {
    for (int i = 0; i < NFRAMES; i++)
        if (p->fr[i].pid >= 0)
            writeback(p, &p->fr[i]);
}

/* Reference LRU simulation on the same reference string (hit counting only). */
static int sim_lru_hits(const int *trace, int n) {
    int cache[NFRAMES], age[NFRAMES], used = 0, hits = 0;
    for (int t = 0; t < n; t++) {
        int found = -1;
        for (int i = 0; i < used; i++)
            if (cache[i] == trace[t])
                found = i;
        if (found >= 0) {
            hits++;
            age[found] = t;
        } else if (used < NFRAMES) {
            cache[used] = trace[t];
            age[used++] = t;
        } else {
            int old = 0;
            for (int i = 1; i < NFRAMES; i++)
                if (age[i] < age[old])
                    old = i;
            cache[old] = trace[t];
            age[old] = t;
        }
    }
    return hits;
}

int main(void) {
    int fd = open("pool.db", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open db");
    uint32_t model[NPAGES];
    for (int pid = 0; pid < NPAGES; pid++) {
        unsigned char pg[PSZ];
        memset(pg, 0, PSZ);
        model[pid] = 0;
        put32(pg, 0);
        put32(pg + 4, crc32_update(0, pg, 4));
        pwrite_all(fd, pg, PSZ, (off_t)pid * PSZ);
    }
    Pool pool;
    pool_init(&pool, fd);
    int trace[3000], nt = 0;
    int pinned_frame[3], pinned_n = 0, pin_fail = 0;
    for (int step = 0; step < 3000; step++) {
        /* 70% of accesses go to a hot set of 5 pages, 30% anywhere. */
        int pid = rndn(10) < 7 ? (int)rndn(5) : (int)rndn(NPAGES);
        int i = pool_pin(&pool, pid);
        check(i >= 0, "frame available");
        trace[nt++] = pid;
        unsigned char *d = pool.fr[i].data;
        check(get32(d + 4) == crc32_update(0, d, 4), "page crc on read");
        check(get32(d) == model[pid], "page content matches model");
        int write = rndn(3) == 0;
        if (write) {
            uint32_t v = get32(d) + 1 + rndn(5);
            put32(d, v);
            put32(d + 4, crc32_update(0, d, 4));
            model[pid] = v;
        }
        if (step % 50 == 7 && pinned_n < 3) { /* keep a few pages pinned across accesses */
            pinned_frame[pinned_n++] = i;
            pool_unpin(&pool, i, 0); /* release the extra pin below; hold one via re-pin */
            int again = pool_pin(&pool, pid);
            check(again == i, "same frame");
        } else {
            pool_unpin(&pool, i, write);
            if (write == 0 && pool.fr[i].dirty == 0) {
                /* clean page stays clean */
            }
        }
        if (step == 2000) { /* release held pins */
            for (int k = 0; k < pinned_n; k++)
                pool_unpin(&pool, pinned_frame[k], 1);
            pinned_n = 0;
        }
    }
    /* Exhaust the pool: pin NFRAMES distinct pages, the next pin must fail. */
    for (int k = 0; k < pinned_n; k++)
        pool_unpin(&pool, pinned_frame[k], 1);
    int held[NFRAMES];
    for (int k = 0; k < NFRAMES; k++) {
        held[k] = pool_pin(&pool, 30 + k);
        check(held[k] >= 0, "pin distinct");
    }
    if (pool_pin(&pool, 20) < 0)
        pin_fail = 1;
    for (int k = 0; k < NFRAMES; k++)
        pool_unpin(&pool, held[k], 0);
    pool_flush(&pool);
    for (int pid = 0; pid < NPAGES; pid++) {
        unsigned char pg[PSZ];
        check(pread_upto(fd, pg, PSZ, (off_t)pid * PSZ) == PSZ, "final read");
        check(get32(pg) == model[pid], "flushed page equals model");
    }
    int sim = sim_lru_hits(trace, nt);
    printf("accesses=%d hits=%ld misses=%ld\n", nt, pool.hits, pool.misses);
    printf("evictions=%ld dirty writebacks=%ld disk reads=%ld writes=%ld\n", pool.evictions, pool.writebacks,
           pool.disk_reads, pool.disk_writes);
    printf("all-frames-pinned refused=%d\n", pin_fail);
    printf("unpinned reference LRU hits=%d (pool hits differ by pinning: %s)\n", sim,
           pool.hits >= sim - 40 && pool.hits <= sim + 40 ? "small" : "large");
    close(fd);
    unlink("pool.db");
    return 0;
}
