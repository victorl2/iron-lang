/*
 * title: Hand-written buffered reader compared against stdio
 * topic: io_files
 * covers: buffered input, refill logic, peek, seek within buffer, line reading, differential test vs FILE
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

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

enum { BUFSZ = 48 };

typedef struct {
    int fd;
    unsigned char buf[BUFSZ];
    size_t pos, end;   /* valid window inside buf */
    off_t base;        /* file offset of buf[0] */
    long refills, hits;
} BufReader;

static void br_init(BufReader *r, int fd) {
    r->fd = fd;
    r->pos = r->end = 0;
    r->base = 0;
    r->refills = r->hits = 0;
}

static off_t br_tell(const BufReader *r) { return r->base + (off_t)r->pos; }

static int br_fill(BufReader *r) {
    r->base += (off_t)r->end;
    r->pos = r->end = 0;
    size_t got = pread_upto(r->fd, r->buf, BUFSZ, r->base);
    r->refills++;
    r->end = got;
    return got > 0;
}

static int br_getc(BufReader *r) {
    if (r->pos == r->end && !br_fill(r))
        return -1;
    return r->buf[r->pos++];
}

static int br_peek(BufReader *r) {
    if (r->pos == r->end && !br_fill(r))
        return -1;
    return r->buf[r->pos];
}

static size_t br_read(BufReader *r, unsigned char *out, size_t n) {
    size_t done = 0;
    while (done < n) {
        if (r->pos == r->end) {
            if (n - done >= BUFSZ) { /* large read bypasses the buffer */
                off_t at = br_tell(r);
                size_t got = pread_upto(r->fd, out + done, n - done, at);
                r->base = at + (off_t)got;
                r->pos = r->end = 0;
                done += got;
                break;
            }
            if (!br_fill(r))
                break;
        }
        size_t take = r->end - r->pos;
        if (take > n - done)
            take = n - done;
        memcpy(out + done, r->buf + r->pos, take);
        r->pos += take;
        done += take;
    }
    return done;
}

/* Like fgets: reads at most cap-1 bytes, stops after '\n'. Returns length or -1 at EOF. */
static int br_gets(BufReader *r, char *out, int cap) {
    int n = 0;
    while (n < cap - 1) {
        int c = br_getc(r);
        if (c < 0)
            break;
        out[n++] = (char)c;
        if (c == '\n')
            break;
    }
    out[n] = 0;
    return n == 0 ? -1 : n;
}

static void br_seek(BufReader *r, off_t to) {
    if (to >= r->base && to <= r->base + (off_t)r->end) {
        r->pos = (size_t)(to - r->base);
        r->hits++;
    } else {
        r->base = to;
        r->pos = r->end = 0;
    }
}

int main(void) {
    unsigned char data[900];
    size_t n = 0;
    while (n < sizeof data - 40) {
        unsigned ll = rndn(30);
        for (unsigned i = 0; i < ll && n < sizeof data - 40; i++)
            data[n++] = (unsigned char)('a' + rndn(26));
        data[n++] = '\n';
    }
    spit("in.txt", data, n);
    int fd = open("in.txt", O_RDONLY);
    FILE *fp = fopen("in.txt", "rb");
    check(fd >= 0 && fp != NULL, "open");
    BufReader r;
    br_init(&r, fd);
    int ops[8] = {0};
    long bytes = 0;
    unsigned digest = 0;
    for (int step = 0; step < 4000; step++) {
        int op = (int)rndn(7);
        ops[op]++;
        switch (op) {
        case 0: { /* getc */
            int a = br_getc(&r), b = fgetc(fp);
            check(a == b, "getc");
            if (a >= 0)
                digest = digest * 33u + (unsigned)a;
            break;
        }
        case 1: { /* peek equals next getc */
            int a = br_peek(&r);
            int b = fgetc(fp);
            if (b != EOF)
                ungetc(b, fp);
            check(a == b, "peek");
            break;
        }
        case 2: { /* read n */
            unsigned char x[200], y[200];
            size_t want = rndn(120);
            size_t a = br_read(&r, x, want), b = fread(y, 1, want, fp);
            check(a == b && memcmp(x, y, a) == 0, "read");
            bytes += (long)a;
            break;
        }
        case 3: { /* gets with small cap */
            char x[24], y[24];
            int cap = 2 + (int)rndn(22);
            int a = br_gets(&r, x, cap);
            char *bres = fgets(y, cap, fp);
            check((a < 0) == (bres == NULL), "gets eof agreement");
            if (a >= 0)
                check(strcmp(x, y) == 0, "gets content");
            break;
        }
        case 4: { /* absolute seek */
            off_t to = (off_t)rndn((uint32_t)n + 1);
            br_seek(&r, to);
            check(fseek(fp, (long)to, SEEK_SET) == 0, "fseek");
            break;
        }
        case 5: { /* relative seek near current position */
            long delta = (long)rndn(40) - 20;
            off_t to = br_tell(&r) + delta;
            if (to < 0)
                to = 0;
            br_seek(&r, to);
            check(fseek(fp, (long)to, SEEK_SET) == 0, "fseek rel");
            break;
        }
        default: /* tell agreement */
            check((long)br_tell(&r) == ftell(fp), "tell");
        }
    }
    check((long)br_tell(&r) == ftell(fp), "final tell");
    printf("file bytes=%zu\n", n);
    printf("ops: getc=%d peek=%d read=%d gets=%d seek=%d relseek=%d tell=%d\n", ops[0], ops[1], ops[2], ops[3],
           ops[4], ops[5], ops[6]);
    printf("bytes via read=%ld digest=%08x\n", bytes, digest);
    printf("refills=%ld in-buffer seeks=%ld\n", r.refills, r.hits);
    fclose(fp);
    close(fd);
    unlink("in.txt");
    return 0;
}
