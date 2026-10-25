/*
 * title: Undo logging with in-place data writes and crash sweep
 * topic: io_files
 * covers: undo log, steal policy, write-ahead rule, reverse rollback, exhaustive crash points, torn log record
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

enum { NSLOT = 24, NTXN = 12 };
#define RECSZ 12 /* type u8, txn u8, slot u8, pad, old u32, crc u32 */
enum { U_UNDO = 1, U_COMMIT = 2 };

typedef struct {
    int kind; /* 0 = log append, 1 = data write */
    int type, txn, slot;
    uint32_t oldv, newv;
} Event;

static Event ev[512];
static int nev;
static uint32_t states[NTXN + 1][NSLOT]; /* state after txn i committed */
static int commit_event[NTXN + 1];       /* event index of the commit append */

static void gen_workload(void) {
    uint32_t cur[NSLOT];
    memset(cur, 0, sizeof cur);
    memcpy(states[0], cur, sizeof cur);
    for (int t = 1; t <= NTXN; t++) {
        int n = 1 + (int)rndn(5);
        for (int u = 0; u < n; u++) {
            int slot = (int)rndn(NSLOT);
            uint32_t nv = 1u + rndn(9999);
            Event a = {0, U_UNDO, t, slot, cur[slot], nv};
            Event d = {1, 0, t, slot, cur[slot], nv};
            ev[nev++] = a; /* write-ahead: undo record first */
            ev[nev++] = d;
            cur[slot] = nv;
        }
        Event c = {0, U_COMMIT, t, 0, 0, 0};
        commit_event[t] = nev;
        ev[nev++] = c;
        memcpy(states[t], cur, sizeof cur);
    }
}

static void put_rec(unsigned char *r, const Event *e) {
    r[0] = (unsigned char)e->type;
    r[1] = (unsigned char)e->txn;
    r[2] = (unsigned char)e->slot;
    r[3] = 0;
    put32(r + 4, e->oldv);
    put32(r + 8, crc32_update(0, r, 8));
}

/* Execute events [0, upto) fully, then optionally `torn` bytes of event upto. */
static void run_until(int upto, int torn) {
    uint32_t zero[NSLOT] = {0};
    unsigned char db[NSLOT * 4];
    for (int i = 0; i < NSLOT; i++)
        put32(db + 4 * i, zero[i]);
    spit("data.db", db, sizeof db);
    int lf = open("undo.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int df = open("data.db", O_RDWR);
    check(lf >= 0 && df >= 0, "open");
    for (int i = 0; i < upto && i < nev; i++) {
        if (ev[i].kind == 0) {
            unsigned char r[RECSZ];
            put_rec(r, &ev[i]);
            write_all(lf, r, RECSZ);
        } else {
            unsigned char v[4];
            put32(v, ev[i].newv);
            pwrite_all(df, v, 4, (off_t)ev[i].slot * 4);
        }
    }
    if (torn > 0 && upto < nev && ev[upto].kind == 0) {
        unsigned char r[RECSZ];
        put_rec(r, &ev[upto]);
        write_all(lf, r, (size_t)torn);
    }
    close(lf);
    close(df);
}

/* Recovery: roll back every txn lacking a commit record, newest record first. */
static int recover(void) {
    size_t n;
    unsigned char *w = slurp("undo.log", &n);
    int nrec = 0;
    struct { int txn, slot; uint32_t old; } recs[512];
    int committed[NTXN + 2] = {0};
    for (size_t pos = 0; pos + RECSZ <= n; pos += RECSZ) {
        if (crc32_update(0, w + pos, 8) != get32(w + pos + 8))
            break;
        if (w[pos] == U_UNDO) {
            recs[nrec].txn = w[pos + 1];
            recs[nrec].slot = w[pos + 2];
            recs[nrec].old = get32(w + pos + 4);
            nrec++;
        } else {
            committed[w[pos + 1]] = 1;
        }
    }
    free(w);
    int df = open("data.db", O_RDWR);
    check(df >= 0, "open data");
    int undone = 0;
    for (int i = nrec - 1; i >= 0; i--) {
        if (committed[recs[i].txn])
            continue;
        unsigned char v[4];
        put32(v, recs[i].old);
        pwrite_all(df, v, 4, (off_t)recs[i].slot * 4);
        undone++;
    }
    close(df);
    return undone;
}

int main(void) {
    gen_workload();
    printf("events=%d txns=%d\n", nev, NTXN);
    int crash_points = 0, total_undone = 0, torn_cases = 0, max_undone = 0;
    for (int upto = 0; upto <= nev; upto++) {
        int variants = (upto < nev && ev[upto].kind == 0) ? 3 : 1;
        for (int v = 0; v < variants; v++) {
            int torn = v == 0 ? 0 : (v == 1 ? 1 : RECSZ - 1);
            run_until(upto, torn);
            int undone = recover();
            /* Expected: state of the last txn whose commit append completed. */
            int last = 0;
            for (int t = 1; t <= NTXN; t++)
                if (commit_event[t] < upto)
                    last = t;
            size_t n;
            unsigned char *b = slurp("data.db", &n);
            check(n == NSLOT * 4, "data size");
            for (int s = 0; s < NSLOT; s++)
                check(get32(b + 4 * s) == states[last][s], "recovered slot value");
            free(b);
            /* A second recovery pass must change nothing. */
            recover();
            b = slurp("data.db", &n);
            for (int s = 0; s < NSLOT; s++)
                check(get32(b + 4 * s) == states[last][s], "idempotent");
            free(b);
            crash_points++;
            total_undone += undone;
            if (undone > max_undone)
                max_undone = undone;
            if (torn)
                torn_cases++;
        }
    }
    printf("crash points=%d (torn log tails=%d)\n", crash_points, torn_cases);
    printf("undo records applied=%d max in one recovery=%d\n", total_undone, max_undone);
    uint32_t h = 0;
    for (int s = 0; s < NSLOT; s++)
        h = h * 131u + states[NTXN][s];
    printf("final state hash=%08x\n", h);
    unlink("undo.log");
    unlink("data.db");
    return 0;
}
