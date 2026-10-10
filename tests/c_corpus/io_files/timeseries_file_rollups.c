/*
 * title: Fixed-interval time-series files with multi-level downsampling rollups
 * topic: io_files
 * covers: O(1) timestamp-to-offset seek, gap sentinels, min/max/sum/count rollups, cascading resolutions, range queries, gmtime formatting
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

#include <time.h>

#define GAP ((int32_t)0x80000000)
enum { HDR = 16 };
/* Raw file: "TSF1" | start u32 | interval u32 | count u32 ; then count x i32 (GAP = missing).
 * Rollup file: same header; then count x { min i32, max i32, sum i64, n u32 } (20 bytes). */
enum { RAW = 4, ROLL = 20 };

typedef struct {
    int32_t min, max;
    int64_t sum;
    uint32_t n;
} Bucket;

static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static void header(unsigned char *h, uint32_t start, uint32_t interval, uint32_t count) {
    memcpy(h, "TSF1", 4);
    put32(h + 4, start);
    put32(h + 8, interval);
    put32(h + 12, count);
}

static void enc_bucket(unsigned char *p, const Bucket *b) {
    put32(p, (uint32_t)b->min);
    put32(p + 4, (uint32_t)b->max);
    put64(p + 8, (uint64_t)b->sum);
    put32(p + 16, b->n);
}
static Bucket dec_bucket(const unsigned char *p) {
    Bucket b;
    b.min = (int32_t)get32(p);
    b.max = (int32_t)get32(p + 4);
    b.sum = (int64_t)get64(p + 8);
    b.n = get32(p + 16);
    return b;
}

static const char *fmt_ts(uint32_t ts) {
    static char buf[32];
    time_t t = (time_t)ts;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

/* Query the raw file by timestamp range [t0, t1): O(1) seek to the first sample. */
static void query_raw(const char *path, uint32_t t0, uint32_t t1, Bucket *out) {
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "open raw");
    unsigned char h[HDR];
    check(pread_upto(fd, h, HDR, 0) == HDR && !memcmp(h, "TSF1", 4), "raw header");
    uint32_t start = get32(h + 4), iv = get32(h + 8), cnt = get32(h + 12);
    uint32_t i0 = t0 <= start ? 0 : (t0 - start + iv - 1) / iv;
    uint32_t i1 = t1 <= start ? 0 : (t1 - start + iv - 1) / iv;
    if (i1 > cnt)
        i1 = cnt;
    memset(out, 0, sizeof *out);
    out->min = INT32_MAX;
    out->max = INT32_MIN + 1;
    for (uint32_t i = i0; i < i1; i++) {
        unsigned char b[4];
        check(pread_upto(fd, b, 4, HDR + (off_t)i * RAW) == 4, "sample");
        int32_t v = (int32_t)get32(b);
        if (v == GAP)
            continue;
        if (v < out->min) out->min = v;
        if (v > out->max) out->max = v;
        out->sum += v;
        out->n++;
    }
    close(fd);
}

static uint32_t make_rollup(const char *src, int src_is_raw, const char *dst, uint32_t factor) {
    size_t n;
    unsigned char *s = slurp(src, &n);
    uint32_t start = get32(s + 4), iv = get32(s + 8), cnt = get32(s + 12);
    uint32_t outn = (cnt + factor - 1) / factor;
    size_t osz = HDR + (size_t)outn * ROLL;
    unsigned char *o = calloc(osz, 1);
    check(o != NULL, "rollup alloc");
    header(o, start, iv * factor, outn);
    for (uint32_t b = 0; b < outn; b++) {
        Bucket acc;
        acc.min = INT32_MAX;
        acc.max = INT32_MIN + 1;
        acc.sum = 0;
        acc.n = 0;
        for (uint32_t k = 0; k < factor && b * factor + k < cnt; k++) {
            uint32_t idx = b * factor + k;
            if (src_is_raw) {
                int32_t v = (int32_t)get32(s + HDR + (size_t)idx * RAW);
                if (v == GAP)
                    continue;
                if (v < acc.min) acc.min = v;
                if (v > acc.max) acc.max = v;
                acc.sum += v;
                acc.n++;
            } else {
                Bucket x = dec_bucket(s + HDR + (size_t)idx * ROLL);
                if (x.n == 0)
                    continue;
                if (x.min < acc.min) acc.min = x.min;
                if (x.max > acc.max) acc.max = x.max;
                acc.sum += x.sum;
                acc.n += x.n;
            }
        }
        enc_bucket(o + HDR + (size_t)b * ROLL, &acc);
    }
    spit(dst, o, osz);
    free(o);
    free(s);
    return outn;
}

int main(void) {
    enum { N = 3 * 3600 + 500 }; /* one sample per second, just over three hours */
    static int32_t data[N];
    const uint32_t start = 1700000000u;
    int gaps = 0;
    int32_t level = 2000;
    for (int i = 0; i < N; i++) {
        level += (int32_t)rndn(21) - 10;
        if (i % 1800 == 0)
            level += 150;
        if (rndn(50) == 0 || (i > 4000 && i < 4090)) {
            data[i] = GAP;
            gaps++;
        } else
            data[i] = level;
    }
    unsigned char *raw = malloc(HDR + (size_t)N * RAW);
    check(raw != NULL, "raw alloc");
    header(raw, start, 1, N);
    for (int i = 0; i < N; i++)
        put32(raw + HDR + (size_t)i * RAW, (uint32_t)data[i]);
    spit("raw.tsf", raw, HDR + (size_t)N * RAW);
    free(raw);
    printf("samples=%d gaps=%d start=%s\n", N, gaps, fmt_ts(start));

    uint32_t n60 = make_rollup("raw.tsf", 1, "m1.tsf", 60);
    uint32_t n3600 = make_rollup("m1.tsf", 0, "h1.tsf", 60);
    printf("rollups: 1-minute buckets=%u, 1-hour buckets=%u\n", n60, n3600);

    /* Cross-check every hourly bucket against a direct query of the raw file. */
    size_t hn;
    unsigned char *h = slurp("h1.tsf", &hn);
    check(get32(h + 8) == 3600, "hour interval");
    for (uint32_t b = 0; b < n3600; b++) {
        Bucket r = dec_bucket(h + HDR + (size_t)b * ROLL), q;
        query_raw("raw.tsf", start + b * 3600u, start + (b + 1) * 3600u, &q);
        check(r.n == q.n && r.sum == q.sum && (r.n == 0 || (r.min == q.min && r.max == q.max)), "hourly rollup");
        printf("%s  n=%4u min=%d max=%d avg=%d\n", fmt_ts(start + b * 3600u), r.n, r.min, r.max,
               r.n ? (int)(r.sum / r.n) : 0);
    }
    free(h);

    /* Arbitrary range queries with brute force check. */
    for (int t = 0; t < 200; t++) {
        uint32_t a = (uint32_t)rndn(N), len = 1 + (uint32_t)rndn(900);
        Bucket q;
        query_raw("raw.tsf", start + a, start + a + len, &q);
        int64_t sum = 0;
        uint32_t cnt = 0;
        for (uint32_t i = a; i < a + len && i < N; i++)
            if (data[i] != GAP) {
                sum += data[i];
                cnt++;
            }
        check(q.n == cnt && q.sum == sum, "range query");
    }
    /* The gap window (samples 4001..4089) produces empty minute buckets. */
    size_t mn;
    unsigned char *m = slurp("m1.tsf", &mn);
    int empty = 0;
    for (uint32_t b = 0; b < n60; b++)
        if (dec_bucket(m + HDR + (size_t)b * ROLL).n == 0)
            empty++;
    free(m);
    printf("empty minute buckets=%d, file sizes raw=%ld m1=%ld h1=%ld\n", empty, file_size("raw.tsf"),
           file_size("m1.tsf"), file_size("h1.tsf"));
    unlink("raw.tsf");
    unlink("m1.tsf");
    unlink("h1.tsf");
    return 0;
}
