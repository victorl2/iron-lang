/*
 * title: Copy-on-write shadow paging with dual superblocks and crash sweep
 * topic: io_files
 * covers: copy-on-write pages, page table indirection, alternating superblocks, atomic root flip, free list reuse, exhaustive crash points
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
enum { PSZ = 64, NLOG = 12, MAXPHYS = 64, NTX = 7 };
/* Layout: page 0,1 = superblock slots; page 2.. = data and page-table pages.
 * Superblock: "SB" | seq u32 | table_page u32 | crc u32.  Table page: NLOG x u32 physical numbers + crc. */

static int fd_db = -1;
static long budget;      /* writes allowed before the simulated crash; <0 = unlimited */
static long writes_done; /* total write attempts */
static int crashed;

static void io_write(const void *buf, uint32_t page, size_t len) {
    writes_done++;
    if (budget == 0) {
        crashed = 1;
        return;
    }
    if (budget > 0)
        budget--;
    pwrite_all(fd_db, buf, len, (off_t)page * PSZ);
}
/* A torn superblock write: only the first half of the page makes it. */
static void io_write_torn(const void *buf, uint32_t page) {
    writes_done++;
    if (budget == 0) {
        crashed = 1;
        return;
    }
    if (budget > 0)
        budget--;
    pwrite_all(fd_db, buf, PSZ / 2, (off_t)page * PSZ);
}

typedef struct {
    uint32_t seq;
    uint32_t table[NLOG]; /* logical -> physical, 0 = unmapped */
    uint32_t table_page;
    unsigned char used[MAXPHYS];
} State;

static uint32_t alloc_page(State *s) {
    for (uint32_t p = 4; p < MAXPHYS; p++)
        if (!s->used[p]) {
            s->used[p] = 1;
            return p;
        }
    check(0, "out of pages");
    return 0;
}

static void fill(unsigned char *pg, uint32_t logical, uint32_t gen) {
    for (int i = 0; i < PSZ; i++)
        pg[i] = (unsigned char)(logical * 41u + gen * 7u + (unsigned)i);
}

/* One transaction: rewrite `n` logical pages via COW, then commit with a new table and superblock. */
static void transaction(State *s, const int *logical, const uint32_t *gen, int n, int torn_sb) {
    uint32_t old_pages[NLOG + 1];
    int nold = 0;
    State next = *s;
    for (int i = 0; i < n; i++) {
        uint32_t np = alloc_page(&next);
        unsigned char pg[PSZ];
        fill(pg, (uint32_t)logical[i], gen[i]);
        io_write(pg, np, PSZ);
        if (next.table[logical[i]])
            old_pages[nold++] = next.table[logical[i]];
        next.table[logical[i]] = np;
    }
    uint32_t tp = alloc_page(&next);
    unsigned char tb[PSZ];
    memset(tb, 0, PSZ);
    for (int i = 0; i < NLOG; i++)
        put32(tb + 4 * i, next.table[i]);
    put32(tb + 4 * NLOG, crc32_update(0, tb, 4 * NLOG));
    io_write(tb, tp, PSZ);
    next.seq = s->seq + 1;
    unsigned char sb[PSZ];
    memset(sb, 0, PSZ);
    memcpy(sb, "SB", 2);
    put32(sb + 4, next.seq);
    put32(sb + 8, tp);
    put32(sb + 12, crc32_update(0, sb, 12));
    /* pad with a checksum over the full page so a torn write is detectable */
    put32(sb + PSZ - 4, crc32_update(0, sb, PSZ - 4));
    if (torn_sb)
        io_write_torn(sb, next.seq & 1);
    else
        io_write(sb, next.seq & 1, PSZ);
    /* commit succeeded in memory: release the old table page and replaced data pages */
    for (int i = 0; i < nold; i++)
        next.used[old_pages[i]] = 0;
    if (s->table_page)
        next.used[s->table_page] = 0;
    next.table_page = tp;
    *s = next;
}

/* Recovery: choose the valid superblock with the highest seq. Fills table; returns seq or -1. */
static long recover(uint32_t *table) {
    long best = -1;
    for (int slot = 0; slot < 2; slot++) {
        unsigned char sb[PSZ];
        if (pread_upto(fd_db, sb, PSZ, (off_t)slot * PSZ) != PSZ)
            continue;
        if (memcmp(sb, "SB", 2) != 0 || get32(sb + PSZ - 4) != crc32_update(0, sb, PSZ - 4))
            continue;
        uint32_t seq = get32(sb + 4), tp = get32(sb + 8);
        unsigned char tb[PSZ];
        if (pread_upto(fd_db, tb, PSZ, (off_t)tp * PSZ) != PSZ || get32(tb + 4 * NLOG) != crc32_update(0, tb, 4 * NLOG))
            continue;
        if ((long)seq > best) {
            best = (long)seq;
            for (int i = 0; i < NLOG; i++)
                table[i] = get32(tb + 4 * i);
        }
    }
    return best;
}

/* Runs the fixed workload; ground truth generation numbers per logical page after each txn. */
static uint32_t truth[NTX + 1][NLOG];

static void workload(long crash_budget, int torn_last) {
    unlink("shadow.db");
    fd_db = open("shadow.db", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd_db >= 0, "open");
    unsigned char zero[PSZ * 4];
    memset(zero, 0, sizeof zero);
    pwrite_all(fd_db, zero, sizeof zero, 0); /* both superblocks invalid, pages 2,3 reserved */
    budget = crash_budget;
    writes_done = 0;
    crashed = 0;
    State s;
    memset(&s, 0, sizeof s);
    s.used[0] = s.used[1] = s.used[2] = s.used[3] = 1;
    uint64_t saved = rng_state;
    rng_state = 0xABCDEF01ULL;
    for (int t = 1; t <= NTX; t++) {
        int n = 1 + (int)rndn(5);
        int logical[8];
        uint32_t gen[8];
        for (int i = 0; i < n; i++) {
            logical[i] = (int)rndn(NLOG);
            gen[i] = (uint32_t)(t * 10 + i);
        }
        /* drop duplicates within the txn: keep the last */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (logical[i] == logical[j])
                    logical[i] = -1;
        int m = 0;
        for (int i = 0; i < n; i++)
            if (logical[i] >= 0) {
                logical[m] = logical[i];
                gen[m++] = gen[i];
            }
        memcpy(truth[t], truth[t - 1], sizeof truth[t]);
        for (int i = 0; i < m; i++)
            truth[t][logical[i]] = gen[i];
        transaction(&s, logical, gen, m, torn_last && t == NTX);
    }
    rng_state = saved;
}

int main(void) {
    memset(truth, 0, sizeof truth);
    workload(-1, 0);
    long total = writes_done;
    close(fd_db);
    printf("transactions=%d total page writes=%ld\n", NTX, total);
    int hist[NTX + 1] = {0};
    int checked = 0;
    for (long b = 0; b <= total; b++) {
        for (int torn = 0; torn < 2; torn++) {
            workload(b, torn);
            uint32_t table[NLOG];
            long seq = recover(table);
            check(seq >= 0 || b < 6, "some txn survives once first commit finished");
            int t = seq < 0 ? 0 : (int)seq;
            /* Every logical page must read back the generation of the recovered txn. */
            for (int l = 0; l < NLOG; l++) {
                if (t == 0) {
                    continue;
                }
                if (truth[t][l] == 0) {
                    check(table[l] == 0, "unmapped page stays unmapped");
                    continue;
                }
                unsigned char pg[PSZ], want[PSZ];
                check(pread_upto(fd_db, pg, PSZ, (off_t)table[l] * PSZ) == PSZ, "data read");
                fill(want, (uint32_t)l, truth[t][l]);
                check(memcmp(pg, want, PSZ) == 0, "page content is from recovered snapshot");
            }
            hist[t]++;
            checked++;
            close(fd_db);
        }
    }
    printf("crash scenarios checked=%d\n", checked);
    for (int t = 0; t <= NTX; t++)
        printf("  recovered txn %d: %d scenarios\n", t, hist[t]);
    unlink("shadow.db");
    return 0;
}
