/*
 * title: Multi-file atomic update with staged files and a commit marker
 * topic: io_files
 * covers: roll-forward recovery, staged .new files, commit marker with checksum, rename sequence, crash at every step, all-or-nothing invariant
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

enum { NF = 4 };
static const char *names[NF] = {"accounts.dat", "index.dat", "meta.dat", "totals.dat"};

static long steps_left; /* crash after this many steps (<0 = never) */
static int crashed;
static int nsteps_taken;

static int step(void) {
    nsteps_taken++;
    if (crashed)
        return 0;
    if (steps_left == 0) {
        crashed = 1;
        return 0;
    }
    if (steps_left > 0)
        steps_left--;
    return 1;
}

/* File image: "V" version u32 | payload 40 bytes derived from (file, version) | crc */
static void image(unsigned char *out, int file, uint32_t ver) {
    out[0] = 'V';
    put32(out + 1, ver);
    for (int i = 0; i < 40; i++)
        out[5 + i] = (unsigned char)(file * 53 + (int)ver * 11 + i);
    put32(out + 45, crc32_update(0, out, 45));
}
#define IMG 49

static int read_version(int file) {
    size_t n;
    if (file_size(names[file]) < 0)
        return -1;
    unsigned char *b = slurp(names[file], &n);
    int v = -2;
    unsigned char want[IMG];
    if (n == IMG && b[0] == 'V') {
        image(want, file, get32(b + 1));
        if (memcmp(want, b, IMG) == 0)
            v = (int)get32(b + 1);
    }
    free(b);
    return v;
}

static void newname(char *o, size_t c, int f) { snprintf(o, c, "%s.new", names[f]); }

/* One transaction to version `ver`. Each visible filesystem action is a step; a crash stops the run.
 * Marker: "TXNC" | ver u32 | nfiles u32 | crc. A torn marker write (step) leaves half the bytes. */
static void run_txn(uint32_t ver) {
    unsigned char img[IMG];
    char nn[64];
    for (int f = 0; f < NF; f++) {
        if (!step())
            return;
        image(img, f, ver);
        newname(nn, sizeof nn, f);
        if (f == 1 && steps_left == 0) { /* partial staged write just before the crash point */
            spit(nn, img, 20);
        } else
            spit(nn, img, IMG);
    }
    unsigned char mk[16];
    memcpy(mk, "TXNC", 4);
    put32(mk + 4, ver);
    put32(mk + 8, NF);
    put32(mk + 12, crc32_update(0, mk, 12));
    if (!step()) {
        spit("COMMIT", mk, 9); /* torn marker */
        return;
    }
    spit("COMMIT", mk, 16);
    for (int f = 0; f < NF; f++) {
        if (!step())
            return;
        newname(nn, sizeof nn, f);
        check(rename(nn, names[f]) == 0, "rename staged");
    }
    if (!step())
        return;
    check(unlink("COMMIT") == 0, "unlink marker");
}

/* Restart: roll forward when a valid marker exists, otherwise discard staged files. */
static const char *recover(void) {
    const char *what = "clean";
    char nn[64];
    if (file_size("COMMIT") >= 0) {
        size_t n;
        unsigned char *m = slurp("COMMIT", &n);
        int valid = n == 16 && !memcmp(m, "TXNC", 4) && get32(m + 12) == crc32_update(0, m, 12);
        free(m);
        if (valid) {
            for (int f = 0; f < NF; f++) {
                newname(nn, sizeof nn, f);
                if (file_size(nn) >= 0)
                    check(rename(nn, names[f]) == 0, "roll forward");
            }
            what = "rolled forward";
        } else
            what = "discarded (bad marker)";
        unlink("COMMIT");
    }
    for (int f = 0; f < NF; f++) {
        newname(nn, sizeof nn, f);
        if (file_size(nn) >= 0) {
            unlink(nn);
            if (!strcmp(what, "clean"))
                what = "discarded (uncommitted)";
        }
    }
    return what;
}

static void reset_files(uint32_t ver) {
    unsigned char img[IMG];
    for (int f = 0; f < NF; f++) {
        image(img, f, ver);
        spit(names[f], img, IMG);
    }
}

int main(void) {
    steps_left = -1;
    crashed = 0;
    nsteps_taken = 0;
    reset_files(1);
    run_txn(2);
    int total_steps = nsteps_taken;
    printf("steps in one transaction=%d\n", total_steps);
    int old_ct = 0, new_ct = 0, fwd = 0, disc = 0, bad_marker = 0;
    for (int crash_at = 0; crash_at <= total_steps; crash_at++) {
        reset_files(1);
        unlink("COMMIT");
        steps_left = crash_at;
        crashed = 0;
        nsteps_taken = 0;
        run_txn(2);
        int before_recovery[NF];
        for (int f = 0; f < NF; f++)
            before_recovery[f] = read_version(f);
        const char *what = recover();
        int v0 = read_version(0);
        for (int f = 1; f < NF; f++)
            check(read_version(f) == v0, "all files share one version after recovery");
        check(v0 == 1 || v0 == 2, "version is old or new");
        check(file_size("COMMIT") < 0, "marker removed");
        /* Before recovery, mixed states are possible only after the marker was durable. */
        int mixed = 0;
        for (int f = 1; f < NF; f++)
            if (before_recovery[f] != before_recovery[0])
                mixed = 1;
        if (mixed)
            check(v0 == 2, "mixed state only when rolling forward");
        if (v0 == 1)
            old_ct++;
        else
            new_ct++;
        if (!strcmp(what, "rolled forward"))
            fwd++;
        else if (!strcmp(what, "discarded (bad marker)"))
            bad_marker++;
        else if (!strcmp(what, "discarded (uncommitted)"))
            disc++;
        printf("crash after step %d: %-24s version=%d%s\n", crash_at, what, v0, mixed ? " (was mixed on disk)" : "");
    }
    printf("old=%d new=%d rolled forward=%d discarded=%d torn marker=%d\n", old_ct, new_ct, fwd, disc, bad_marker);
    /* A chain of transactions with random crash points ends in a consistent state. */
    reset_files(1);
    uint32_t ver = 1;
    int crashes = 0;
    for (int round = 0; round < 40; round++) {
        steps_left = (long)rndn(12) - 1;
        crashed = 0;
        run_txn(ver + 1);
        recover();
        int v0 = read_version(0);
        for (int f = 1; f < NF; f++)
            check(read_version(f) == v0, "chain consistency");
        if (crashed)
            crashes++;
        check(v0 == (int)ver || v0 == (int)ver + 1, "chain version step");
        ver = (uint32_t)v0;
    }
    printf("chain of 40 attempts with %d crashes ends at version %u\n", crashes, ver);
    for (int f = 0; f < NF; f++)
        unlink(names[f]);
    return 0;
}
