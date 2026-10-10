/*
 * title: Atomic file replacement versus in-place overwrite under torn writes
 * topic: io_files
 * covers: temp file + fsync + rename, torn write simulation, checksummed container, stale temp cleanup, crash sweep
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

#include <dirent.h>

/* Container: "CFG1" | version u32 | len u32 | payload | crc32 of everything before */
static size_t build(unsigned char *out, uint32_t version, unsigned len, unsigned salt) {
    memcpy(out, "CFG1", 4);
    put32(out + 4, version);
    put32(out + 8, len);
    for (unsigned i = 0; i < len; i++)
        out[12 + i] = (unsigned char)((i * 7u + salt * 13u + version) & 0xFF);
    put32(out + 12 + len, crc32_update(0, out, 12 + len));
    return 16u + len;
}

/* Returns version, or -1 (missing), -2 (invalid). */
static long validate(const char *path) {
    if (file_size(path) < 0)
        return -1;
    size_t n;
    unsigned char *b = slurp(path, &n);
    long r = -2;
    if (n >= 16 && memcmp(b, "CFG1", 4) == 0) {
        uint32_t len = get32(b + 8);
        if ((size_t)len + 16 == n && crc32_update(0, b, 12 + len) == get32(b + 12 + len))
            r = (long)get32(b + 4);
    }
    free(b);
    return r;
}

/* Crash model: only `limit` bytes of the new contents reach the file. */
static void write_partial(const char *path, int flags, const unsigned char *buf, size_t n, size_t limit) {
    int fd = open(path, flags, 0644);
    check(fd >= 0, "open partial");
    write_all(fd, buf, limit < n ? limit : n);
    if (limit >= n)
        check(fsync(fd) == 0, "fsync");
    close(fd);
}

static int count_tmp(void) {
    DIR *d = opendir(".");
    int n = 0;
    struct dirent *e;
    check(d != NULL, "opendir");
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcmp(e->d_name + l - 4, ".tmp") == 0)
            n++;
    }
    closedir(d);
    return n;
}

static int cleanup_tmp(void) {
    DIR *d = opendir(".");
    struct dirent *e;
    char names[16][64];
    int n = 0;
    check(d != NULL, "opendir");
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcmp(e->d_name + l - 4, ".tmp") == 0 && n < 16) {
            snprintf(names[n], sizeof names[n], "%s", e->d_name);
            n++;
        }
    }
    closedir(d);
    for (int i = 0; i < n; i++)
        unlink(names[i]);
    return n;
}

int main(void) {
    unsigned char oldb[512], newb[512];
    unsigned oldlen = 150, newlen = 190;
    size_t on = build(oldb, 1, oldlen, 3);
    size_t nn = build(newb, 2, newlen, 9);
    printf("old image=%zu bytes new image=%zu bytes\n", on, nn);

    int inplace_bad = 0, inplace_old = 0, inplace_new = 0;
    for (size_t k = 0; k <= nn; k++) {
        spit("cfg.dat", oldb, on);
        /* in-place: truncate and rewrite, torn after k bytes */
        write_partial("cfg.dat", O_WRONLY | O_TRUNC, newb, nn, k);
        long v = validate("cfg.dat");
        if (v == 1)
            inplace_old++;
        else if (v == 2)
            inplace_new++;
        else
            inplace_bad++;
    }
    /* Overwrite without truncate: old bytes past the torn point survive but the header is wrong. */
    int overwrite_bad = 0;
    for (size_t k = 0; k <= nn; k++) {
        spit("cfg.dat", oldb, on);
        write_partial("cfg.dat", O_WRONLY, newb, nn, k);
        long v = validate("cfg.dat");
        if (v < 0)
            overwrite_bad++;
    }
    printf("in-place truncate: old=%d new=%d corrupt=%d of %zu crash points\n", inplace_old, inplace_new,
           inplace_bad, nn + 1);
    printf("in-place overwrite: corrupt=%d of %zu crash points\n", overwrite_bad, nn + 1);

    int at_old = 0, at_new = 0, at_bad = 0, stale = 0, cleaned = 0;
    /* atomic: write tmp (torn after k bytes), then optionally rename */
    for (size_t k = 0; k <= nn + 1; k++) {
        spit("cfg.dat", oldb, on);
        write_partial("cfg.tmp", O_WRONLY | O_CREAT | O_TRUNC, newb, nn, k);
        if (k >= nn + 1) /* fully written, fsynced, and renamed */
            check(rename("cfg.tmp", "cfg.dat") == 0, "rename");
        long v = validate("cfg.dat");
        if (v == 1)
            at_old++;
        else if (v == 2)
            at_new++;
        else
            at_bad++;
        stale += count_tmp();
        cleaned += cleanup_tmp();
        check(count_tmp() == 0, "tmp cleaned");
    }
    printf("atomic replace: old=%d new=%d corrupt=%d of %zu crash points\n", at_old, at_new, at_bad, nn + 2);
    printf("stale temp files seen=%d removed on restart=%d\n", stale, cleaned);
    check(at_bad == 0 && at_new == 1 && at_old == (int)nn + 1, "atomic outcomes");
    check(inplace_bad > 0, "in-place is unsafe");

    /* A run of 20 successive atomic updates always ends on the last version. */
    spit("cfg.dat", oldb, on);
    long last = 0;
    for (uint32_t ver = 2; ver < 22; ver++) {
        unsigned char buf[512];
        unsigned len = 20 + rndn(200);
        size_t n = build(buf, ver, len, ver);
        write_partial("cfg.tmp", O_WRONLY | O_CREAT | O_TRUNC, buf, n, n);
        check(rename("cfg.tmp", "cfg.dat") == 0, "rename loop");
        last = validate("cfg.dat");
        check(last == (long)ver, "version monotonic");
    }
    printf("final version after 20 atomic updates=%ld\n", last);
    unlink("cfg.dat");
    return 0;
}
