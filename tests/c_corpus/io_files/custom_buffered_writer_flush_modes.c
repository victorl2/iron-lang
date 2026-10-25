/*
 * title: Hand-written buffered writer with full, line and no buffering
 * topic: io_files
 * covers: buffered output, flush policies, write syscall counting, large-write bypass, differential test vs FILE
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
#include <stdarg.h>

enum { BUFSZ = 64 };
typedef enum { M_FULL, M_LINE, M_NONE } Mode;

typedef struct {
    int fd;
    Mode mode;
    unsigned char buf[BUFSZ];
    size_t len;
    long syscalls;
    long flushed; /* bytes handed to the kernel */
} BufWriter;

static void bw_init(BufWriter *w, int fd, Mode m) {
    w->fd = fd;
    w->mode = m;
    w->len = 0;
    w->syscalls = 0;
    w->flushed = 0;
}

static void bw_raw(BufWriter *w, const void *p, size_t n) {
    if (n == 0)
        return;
    write_all(w->fd, p, n);
    w->syscalls++;
    w->flushed += (long)n;
}

static void bw_flush(BufWriter *w) {
    bw_raw(w, w->buf, w->len);
    w->len = 0;
}

static void bw_append(BufWriter *w, const unsigned char *p, size_t n) {
    while (n > 0) {
        if (w->len == 0 && n >= BUFSZ) { /* big chunk bypasses the buffer */
            bw_raw(w, p, n);
            return;
        }
        size_t room = BUFSZ - w->len;
        size_t take = n < room ? n : room;
        memcpy(w->buf + w->len, p, take);
        w->len += take;
        p += take;
        n -= take;
        if (w->len == BUFSZ)
            bw_flush(w);
    }
}

static void bw_write(BufWriter *w, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    if (w->mode == M_NONE) {
        bw_raw(w, p, n);
        return;
    }
    if (w->mode == M_LINE) {
        size_t upto = 0; /* one past the last newline */
        for (size_t i = 0; i < n; i++)
            if (p[i] == '\n')
                upto = i + 1;
        if (upto > 0) {
            bw_append(w, p, upto);
            bw_flush(w);
            p += upto;
            n -= upto;
        }
    }
    bw_append(w, p, n);
}

static void bw_printf(BufWriter *w, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    check(k >= 0 && (size_t)k < sizeof tmp, "format fits");
    bw_write(w, tmp, (size_t)k);
}

static const char *mode_name(Mode m) { return m == M_FULL ? "full" : m == M_LINE ? "line" : "none"; }

int main(void) {
    static const Mode modes[3] = {M_FULL, M_LINE, M_NONE};
    for (int mi = 0; mi < 3; mi++) {
        Mode m = modes[mi];
        rng_state = 0x1234567ULL; /* same script for every mode */
        int fd = open("mine.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        FILE *fp = fopen("ref.out", "wb");
        check(fd >= 0 && fp != NULL, "open");
        BufWriter w;
        bw_init(&w, fd, m);
        int flushes = 0;
        for (int step = 0; step < 300; step++) {
            int op = (int)rndn(6);
            char s[160];
            switch (op) {
            case 0: { /* short string */
                unsigned l = 1 + rndn(9);
                for (unsigned i = 0; i < l; i++)
                    s[i] = (char)('a' + rndn(26));
                bw_write(&w, s, l);
                fwrite(s, 1, l, fp);
                break;
            }
            case 1: { /* line */
                unsigned l = rndn(30);
                for (unsigned i = 0; i < l; i++)
                    s[i] = (char)('A' + rndn(26));
                s[l] = '\n';
                bw_write(&w, s, l + 1);
                fwrite(s, 1, l + 1, fp);
                break;
            }
            case 2: { /* formatted */
                int v = (int)rndn(100000);
                bw_printf(&w, "[%05d:%x]", v, (unsigned)v);
                fprintf(fp, "[%05d:%x]", v, (unsigned)v);
                break;
            }
            case 3: { /* large block */
                unsigned l = 60 + rndn(100);
                for (unsigned i = 0; i < l; i++)
                    s[i] = (char)('0' + (i % 10));
                bw_write(&w, s, l);
                fwrite(s, 1, l, fp);
                break;
            }
            case 4: /* explicit flush */
                bw_flush(&w);
                fflush(fp);
                flushes++;
                break;
            default: { /* single byte */
                unsigned char c = (unsigned char)('!' + rndn(60));
                bw_write(&w, &c, 1);
                fputc(c, fp);
            }
            }
            if (op == 4) {
                check(file_size("mine.out") == file_size("ref.out"), "sizes equal after flush");
                check(w.flushed == file_size("mine.out"), "flushed counter");
            }
        }
        bw_flush(&w);
        fclose(fp);
        close(fd);
        size_t a, b;
        unsigned char *x = slurp("mine.out", &a), *y = slurp("ref.out", &b);
        check(a == b && memcmp(x, y, a) == 0, "final contents equal");
        uint32_t crc = crc32_update(0, x, a);
        free(x);
        free(y);
        printf("%-4s mode: bytes=%zu write calls=%ld explicit flushes=%d crc=%08x\n", mode_name(m), a, w.syscalls,
               flushes, crc);
    }
    unlink("mine.out");
    unlink("ref.out");
    return 0;
}
