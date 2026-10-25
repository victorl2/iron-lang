/*
 * title: Append-only log recovery at every truncation offset
 * topic: io_files
 * covers: append-only log, torn tail detection, crc framing, truncate sweep, recovery then append
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

static inline void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static inline unsigned get16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
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

/* Frame: 'L' 'G' | len u16 | payload | crc32(len+payload) */
enum { HDR = 4, TRL = 4, NREC = 24 };

static size_t frame(unsigned char *out, const unsigned char *pl, unsigned len) {
    out[0] = 'L';
    out[1] = 'G';
    put16(out + 2, len);
    memcpy(out + HDR, pl, len);
    put32(out + HDR + len, crc32_update(0, out + 2, 2u + len));
    return HDR + len + TRL;
}

/* Returns number of valid records and the offset just after the last one. */
static int scan(const unsigned char *b, size_t n, size_t *good_end) {
    size_t pos = 0;
    int cnt = 0;
    while (pos + HDR + TRL <= n) {
        if (b[pos] != 'L' || b[pos + 1] != 'G')
            break;
        unsigned len = get16(b + pos + 2);
        if (pos + HDR + len + TRL > n)
            break;
        uint32_t want = get32(b + pos + HDR + len);
        if (crc32_update(0, b + pos + 2, 2u + len) != want)
            break;
        pos += HDR + len + TRL;
        cnt++;
    }
    *good_end = pos;
    return cnt;
}

int main(void) {
    unsigned char log[4096];
    size_t ends[NREC + 1];
    size_t total = 0;
    ends[0] = 0;
    for (int i = 0; i < NREC; i++) {
        unsigned char pl[64];
        unsigned len = (i % 6 == 5) ? 0u : 1u + rndn(40);
        for (unsigned k = 0; k < len; k++)
            pl[k] = (unsigned char)('a' + (i + k) % 26);
        total += frame(log + total, pl, len);
        ends[i + 1] = total;
    }
    printf("log bytes=%zu records=%d\n", total, NREC);

    spit("wal.log", log, total);
    int hist_full = 0, hist_partial = 0;
    size_t worst_lost = 0;
    for (size_t cut = 0; cut <= total; cut++) {
        check(truncate("wal.log", (off_t)cut) == 0, "truncate");
        size_t n;
        unsigned char *b = slurp("wal.log", &n);
        size_t good;
        int cnt = scan(b, n, &good);
        free(b);
        int expect = 0;
        while (expect < NREC && ends[expect + 1] <= cut)
            expect++;
        check(cnt == expect, "recovered count equals complete-record count");
        check(good == ends[expect], "good end offset");
        if (good == cut)
            hist_full++;
        else {
            hist_partial++;
            if (cut - good > worst_lost)
                worst_lost = cut - good;
        }
        /* Recovery: chop torn tail then append a fresh record and rescan. */
        check(truncate("wal.log", (off_t)good) == 0, "truncate to good");
        int fd = open("wal.log", O_WRONLY | O_APPEND);
        check(fd >= 0, "open append");
        unsigned char fr[64];
        size_t fl = frame(fr, (const unsigned char *)"NEW", 3);
        write_all(fd, fr, fl);
        close(fd);
        b = slurp("wal.log", &n);
        size_t g2;
        int c2 = scan(b, n, &g2);
        free(b);
        check(c2 == expect + 1 && g2 == n, "append after recovery");
        /* Restore original for the next cut. */
        spit("wal.log", log, cut < total ? total : total);
    }
    printf("cuts at record boundary=%d, mid-record=%d\n", hist_full, hist_partial);
    printf("largest torn tail=%zu bytes\n", worst_lost);

    /* Single-bit corruption: flipping any payload bit must stop the scan at that record. */
    int stops[NREC + 1] = {0};
    for (size_t bit = 0; bit < total * 8; bit += 37) {
        unsigned char tmp[4096];
        memcpy(tmp, log, total);
        tmp[bit / 8] ^= (unsigned char)(1u << (bit % 8));
        size_t good;
        int cnt = scan(tmp, total, &good);
        int rec = 0;
        while (rec < NREC && ends[rec + 1] <= bit / 8)
            rec++;
        check(cnt == rec, "corruption stops at damaged record");
        stops[cnt]++;
    }
    int first = -1, last = -1, used = 0;
    for (int i = 0; i < NREC; i++)
        if (stops[i]) {
            if (first < 0)
                first = i;
            last = i;
            used += stops[i];
        }
    printf("bit flips stopped scan: %d flips over records %d..%d\n", used, first, last);
    unlink("wal.log");
    return 0;
}
