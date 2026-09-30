/*
 * title: Write-ahead log with redo-only recovery
 * topic: io_files
 * covers: write-ahead logging, redo recovery, no-steal/no-force, checkpoints, torn log tail, idempotent replay
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

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer (caller frees). */
static inline unsigned char *slurp(const char *path, size_t *len) {
    long n = file_size(path);
    check(n >= 0, "slurp stat");
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    check(b != NULL, "malloc");
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "slurp open");
    size_t got = pread_upto(fd, b, (size_t)n, 0);
    close(fd);
    check(got == (size_t)n, "slurp short");
    *len = (size_t)n;
    return b;
}

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

enum { NPAGE = 16, MAXREC = 4096 };
enum { R_BEGIN = 1, R_UPDATE = 2, R_COMMIT = 3 };

/* Record: type u8 | txn u8 | page u8 | pad u8 | value u32 | crc u32 (12 bytes) */
#define RECSZ 12

static int wal_fd = -1;
static long wal_len = 0;

static void wal_append(int type, int txn, int page, uint32_t value) {
    unsigned char r[RECSZ];
    r[0] = (unsigned char)type;
    r[1] = (unsigned char)txn;
    r[2] = (unsigned char)page;
    r[3] = 0;
    put32(r + 4, value);
    put32(r + 8, crc32_update(0, r, 8));
    write_all(wal_fd, r, RECSZ);
    wal_len += RECSZ;
}

static void load_data(uint32_t *pages) {
    unsigned char buf[NPAGE * 4];
    int fd = open("data.db", O_RDONLY);
    check(fd >= 0, "data open");
    size_t got = pread_upto(fd, buf, sizeof buf, 0);
    close(fd);
    check(got == sizeof buf, "data size");
    for (int i = 0; i < NPAGE; i++)
        pages[i] = get32(buf + 4 * i);
}

static void store_data(const uint32_t *pages) {
    unsigned char buf[NPAGE * 4];
    for (int i = 0; i < NPAGE; i++)
        put32(buf + 4 * i, pages[i]);
    spit("data.db", buf, sizeof buf);
}

/* Redo recovery: returns number of committed txns replayed. */
static int recover(uint32_t *pages, int *torn_bytes) {
    size_t n;
    unsigned char *w = slurp("wal.log", &n);
    struct { unsigned char page, txn; uint32_t v; } pend[MAXREC];
    int npend = 0, ncommit = 0;
    size_t pos = 0;
    while (pos + RECSZ <= n) {
        if (crc32_update(0, w + pos, 8) != get32(w + pos + 8))
            break;
        int type = w[pos], txn = w[pos + 1];
        if (type == R_UPDATE) {
            pend[npend].page = w[pos + 2];
            pend[npend].txn = (unsigned char)txn;
            pend[npend].v = get32(w + pos + 4);
            npend++;
        } else if (type == R_COMMIT) {
            for (int i = 0; i < npend; i++)
                if (pend[i].txn == txn)
                    pages[pend[i].page] = pend[i].v;
            for (int i = 0; i < npend; i++)
                if (pend[i].txn == txn)
                    pend[i].txn = 255;
            ncommit++;
        }
        pos += RECSZ;
    }
    *torn_bytes = (int)(n - pos);
    free(w);
    return ncommit;
}

int main(void) {
    int total_commits = 0, total_lost_txns = 0, torn_trials = 0, ckpts = 0;
    unsigned digest = 0;
    for (int trial = 0; trial < 60; trial++) {
        uint32_t disk[NPAGE], model[NPAGE], mem[NPAGE];
        memset(disk, 0, sizeof disk);
        store_data(disk);
        wal_fd = open("wal.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        check(wal_fd >= 0, "wal open");
        wal_len = 0;
        memset(model, 0, sizeof model);
        memcpy(mem, disk, sizeof mem);
        /* commit history: (end offset of commit record, page, value) sequence */
        struct { long end; int n; int page[4]; uint32_t val[4]; } hist[64];
        int nh = 0;
        uint32_t ckpt_state[NPAGE];
        memset(ckpt_state, 0, sizeof ckpt_state);
        int nsteps = 8 + (int)rndn(20);
        int open_txn = -1;
        int pend_page[4] = {0}, pend_n = 0;
        uint32_t pend_val[4] = {0};
        for (int s = 0; s < nsteps; s++) {
            /* Even steps touch even pages and always commit; odd steps touch odd pages and may stay open. */
            int par = s & 1, txn = s + 1;
            int nup = 1 + (int)rndn(3);
            int pg[4];
            uint32_t vv[4];
            wal_append(R_BEGIN, txn, 0, 0);
            for (int u = 0; u < nup; u++) {
                pg[u] = (int)rndn(NPAGE / 2) * 2 + par;
                vv[u] = (uint32_t)rndn(1000);
                wal_append(R_UPDATE, txn, pg[u], vv[u]);
            }
            int commit = (par == 0) || rndn(3) != 0;
            if (commit) {
                wal_append(R_COMMIT, txn, 0, 0);
                hist[nh].end = wal_len;
                hist[nh].n = nup;
                for (int u = 0; u < nup; u++) {
                    hist[nh].page[u] = pg[u];
                    hist[nh].val[u] = vv[u];
                }
                nh++;
            } else {
                open_txn = txn;
                pend_n = nup;
                memcpy(pend_page, pg, sizeof pend_page);
                memcpy(pend_val, vv, sizeof pend_val);
            }
            if (s == nsteps / 2 && trial % 3 == 0 && open_txn < 0) {
                /* checkpoint: flush all committed state, restart the log */
                for (int h = 0; h < nh; h++)
                    for (int u = 0; u < hist[h].n; u++)
                        ckpt_state[hist[h].page[u]] = hist[h].val[u];
                store_data(ckpt_state);
                close(wal_fd);
                wal_fd = open("wal.log", O_WRONLY | O_TRUNC);
                wal_len = 0;
                nh = 0;
                ckpts++;
            }
        }
        (void)pend_page; (void)pend_val; (void)pend_n;
        close(wal_fd);
        /* Crash: tear the log at an arbitrary byte (sometimes exactly at the end). */
        long cut = (trial % 4 == 0) ? wal_len : (long)rndn((uint32_t)wal_len + 1);
        check(truncate("wal.log", (off_t)cut) == 0, "tear");
        /* Expected state = checkpointed state + commits whose commit record survived. */
        memcpy(model, ckpt_state, sizeof model);
        int expect_commits = 0;
        for (int h = 0; h < nh; h++)
            if (hist[h].end <= cut) {
                for (int u = 0; u < hist[h].n; u++)
                    model[hist[h].page[u]] = hist[h].val[u];
                expect_commits++;
            }
        load_data(mem);
        int torn;
        int got = recover(mem, &torn);
        check(got == expect_commits, "committed txn count");
        check(memcmp(mem, model, sizeof mem) == 0, "state after redo");
        check(torn == (int)(cut % RECSZ), "torn bytes");
        /* Crash during recovery: replay again from the same start; must be idempotent. */
        uint32_t mem2[NPAGE];
        load_data(mem2);
        int t2;
        recover(mem2, &t2);
        recover(mem2, &t2);
        check(memcmp(mem2, model, sizeof mem2) == 0, "idempotent redo");
        total_commits += got;
        total_lost_txns += nh - expect_commits;
        if (torn)
            torn_trials++;
        for (int i = 0; i < NPAGE; i++)
            digest = digest * 31u + mem[i];
    }
    printf("trials=60 checkpoints=%d\n", ckpts);
    printf("commits replayed=%d lost to tear=%d trials with torn tail=%d\n", total_commits, total_lost_txns, torn_trials);
    printf("state digest=%08x\n", digest);
    unlink("wal.log");
    unlink("data.db");
    return 0;
}
