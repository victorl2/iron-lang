/*
 * title: File page cache with pluggable FIFO, LRU, CLOCK and optimal eviction
 * topic: io_files
 * covers: replacement policies, function-pointer strategy, second-chance reference bits, Belady optimal bound, scan resistance, real page reads
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
enum { PSZ = 32, NPAGES = 64, FRAMES = 8, TRACE = 1500 };

typedef struct Cache Cache;
struct Cache {
    int fd;
    int page[FRAMES];      /* -1 = empty */
    int loaded_at[FRAMES]; /* tick when loaded (FIFO) */
    int used_at[FRAMES];   /* tick of last use (LRU) */
    int ref[FRAMES];       /* reference bit (CLOCK) */
    int hand;
    int tick;
    long hits, misses, disk_reads;
    const int *trace;      /* for OPT */
    int trace_len;
    int (*victim)(Cache *c);
};

static int find(const Cache *c, int p) {
    for (int i = 0; i < FRAMES; i++)
        if (c->page[i] == p)
            return i;
    return -1;
}

static int victim_fifo(Cache *c) {
    int v = 0;
    for (int i = 1; i < FRAMES; i++)
        if (c->loaded_at[i] < c->loaded_at[v])
            v = i;
    return v;
}
static int victim_lru(Cache *c) {
    int v = 0;
    for (int i = 1; i < FRAMES; i++)
        if (c->used_at[i] < c->used_at[v])
            v = i;
    return v;
}
static int victim_clock(Cache *c) {
    for (;;) {
        int i = c->hand;
        c->hand = (c->hand + 1) % FRAMES;
        if (c->ref[i]) {
            c->ref[i] = 0;
            continue;
        }
        return i;
    }
}
static int victim_opt(Cache *c) {
    int best = 0, best_next = -1;
    for (int i = 0; i < FRAMES; i++) {
        int nxt = c->trace_len + 1;
        for (int t = c->tick + 1; t < c->trace_len; t++)
            if (c->trace[t] == c->page[i]) {
                nxt = t;
                break;
            }
        if (nxt > best_next) {
            best_next = nxt;
            best = i;
        }
    }
    return best;
}

static void access_page(Cache *c, int p) {
    int i = find(c, p);
    if (i >= 0) {
        c->hits++;
        c->used_at[i] = c->tick;
        c->ref[i] = 1;
        return;
    }
    c->misses++;
    int slot = find(c, -1);
    if (slot < 0)
        slot = c->victim(c);
    unsigned char pg[PSZ];
    check(pread_upto(c->fd, pg, PSZ, (off_t)p * PSZ) == PSZ, "page read");
    check(get32(pg) == (uint32_t)p * 2654435761u, "page holds its own number");
    c->disk_reads++;
    c->page[slot] = p;
    c->loaded_at[slot] = c->tick;
    c->used_at[slot] = c->tick;
    c->ref[slot] = 1;
}

static void run_policy(const char *name, int (*victim)(Cache *), const int *trace, int n, int fd, long *hits_out,
                       long *reads_out) {
    (void)name;
    Cache c;
    memset(&c, 0, sizeof c);
    c.fd = fd;
    for (int i = 0; i < FRAMES; i++)
        c.page[i] = -1;
    c.trace = trace;
    c.trace_len = n;
    c.victim = victim;
    for (c.tick = 0; c.tick < n; c.tick++)
        access_page(&c, trace[c.tick]);
    check(c.hits + c.misses == n, "accounting");
    *hits_out = c.hits;
    *reads_out = c.disk_reads;
}

int main(void) {
    int fd = open("pages.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "open");
    for (int p = 0; p < NPAGES; p++) {
        unsigned char pg[PSZ];
        memset(pg, (int)p, PSZ);
        put32(pg, (uint32_t)p * 2654435761u);
        pwrite_all(fd, pg, PSZ, (off_t)p * PSZ);
    }
    static int trace[TRACE];
    static const char *names[4] = {"looping (12 pages)", "hot set + scan", "skewed random", "phases"};
    static const char *pol[4] = {"FIFO", "LRU", "CLOCK", "OPT"};
    int (*fns[4])(Cache *) = {victim_fifo, victim_lru, victim_clock, victim_opt};
    printf("%-20s %6s %6s %6s %6s\n", "workload", pol[0], pol[1], pol[2], pol[3]);
    for (int w = 0; w < 4; w++) {
        for (int i = 0; i < TRACE; i++) {
            int p;
            switch (w) {
            case 0:
                p = i % 12;
                break;
            case 1:
                /* hot set of 5 pages interleaved with a long sequential scan */
                p = (i % 3 == 0) ? 20 + (i / 3) % 40 : (int)rndn(5);
                break;
            case 2: {
                uint32_t a = rndn(NPAGES), b = rndn(NPAGES);
                p = (int)(a < b ? a : b);
                p = p * p / NPAGES;
                break;
            }
            default: /* three phases with different working sets */
                p = (i / 500) * 20 + (int)rndn(10);
            }
            trace[i] = p;
        }
        long hits[4], reads[4];
        for (int k = 0; k < 4; k++)
            run_policy(pol[k], fns[k], trace, TRACE, fd, &hits[k], &reads[k]);
        check(hits[3] >= hits[0] && hits[3] >= hits[1] && hits[3] >= hits[2], "OPT is an upper bound");
        for (int k = 0; k < 4; k++)
            check(reads[k] == TRACE - hits[k], "one disk read per miss");
        printf("%-20s %6ld %6ld %6ld %6ld\n", names[w], hits[0], hits[1], hits[2], hits[3]);
    }
    printf("cache frames=%d pages=%d accesses per workload=%d\n", FRAMES, NPAGES, TRACE);
    close(fd);
    unlink("pages.dat");
    return 0;
}
