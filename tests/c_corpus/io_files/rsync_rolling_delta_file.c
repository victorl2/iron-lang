/*
 * title: Rsync-style delta between file versions with rolling weak checksum
 * topic: io_files
 * covers: rolling checksum, block signatures, strong hash confirmation, copy/literal delta ops, delta file format, patch apply
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

enum { BS = 64 };

typedef struct {
    uint32_t weak;
    uint64_t strong;
} Sig;

static uint32_t weak_sum(const unsigned char *p, size_t n) {
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        a += p[i];
        b += (uint32_t)(n - i) * p[i];
    }
    return (a & 0xFFFF) | ((b & 0xFFFF) << 16);
}
static uint32_t weak_roll(uint32_t w, unsigned char out, unsigned char in, size_t n) {
    uint32_t a = w & 0xFFFF, b = w >> 16;
    a = (a - out + in) & 0xFFFF;
    b = (b - (uint32_t)n * out + a) & 0xFFFF;
    return a | (b << 16);
}
static uint64_t strong(const unsigned char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
        h ^= h >> 29;
    }
    return h;
}

typedef struct {
    unsigned char *buf;
    size_t len, cap;
    long copies, literals, lit_bytes;
} Delta;

static void d_put(Delta *d, const void *p, size_t n) {
    if (d->len + n > d->cap) {
        d->cap = (d->len + n) * 2 + 64;
        d->buf = realloc(d->buf, d->cap);
        check(d->buf != NULL, "realloc");
    }
    memcpy(d->buf + d->len, p, n);
    d->len += n;
}
static void d_varint(Delta *d, uint32_t v) {
    unsigned char b;
    do {
        b = v & 0x7F;
        v >>= 7;
        if (v)
            b |= 0x80;
        d_put(d, &b, 1);
    } while (v);
}
/* op: 0 = end, 1 = copy block idx, 2 = literal run */
static void d_copy(Delta *d, uint32_t idx) {
    d_put(d, "\x01", 1);
    d_varint(d, idx);
    d->copies++;
}
static void d_lit(Delta *d, const unsigned char *p, size_t n) {
    if (n == 0)
        return;
    d_put(d, "\x02", 1);
    d_varint(d, (uint32_t)n);
    d_put(d, p, n);
    d->literals++;
    d->lit_bytes += (long)n;
}

static void make_delta(const unsigned char *old, size_t no, const unsigned char *nw, size_t nn, Delta *d) {
    size_t nb = no / BS; /* only whole blocks are signed */
    Sig *sig = malloc((nb + 1) * sizeof *sig);
    check(sig != NULL, "sig");
    for (size_t i = 0; i < nb; i++) {
        sig[i].weak = weak_sum(old + i * BS, BS);
        sig[i].strong = strong(old + i * BS, BS);
    }
    memset(d, 0, sizeof *d);
    size_t pos = 0, lit_start = 0;
    uint32_t w = 0;
    int have = 0;
    while (pos + BS <= nn) {
        if (!have) {
            w = weak_sum(nw + pos, BS);
            have = 1;
        }
        long hit = -1;
        for (size_t i = 0; i < nb; i++)
            if (sig[i].weak == w && sig[i].strong == strong(nw + pos, BS)) {
                hit = (long)i;
                break;
            }
        if (hit >= 0) {
            d_lit(d, nw + lit_start, pos - lit_start);
            d_copy(d, (uint32_t)hit);
            pos += BS;
            lit_start = pos;
            have = 0;
        } else {
            if (pos + BS < nn)
                w = weak_roll(w, nw[pos], nw[pos + BS], BS);
            pos++;
        }
    }
    d_lit(d, nw + lit_start, nn - lit_start);
    d_put(d, "\x00", 1);
    free(sig);
}

static size_t apply_delta(const unsigned char *old, size_t no, const unsigned char *dl, size_t nd, unsigned char *out) {
    size_t p = 0, o = 0;
    for (;;) {
        check(p < nd, "delta truncated");
        int op = dl[p++];
        if (op == 0)
            break;
        uint32_t v = 0;
        int sh = 0;
        for (;;) {
            unsigned char b = dl[p++];
            v |= (uint32_t)(b & 0x7F) << sh;
            sh += 7;
            if (!(b & 0x80))
                break;
        }
        if (op == 1) {
            check((size_t)(v + 1) * BS <= no, "copy in range");
            memcpy(out + o, old + (size_t)v * BS, BS);
            o += BS;
        } else {
            memcpy(out + o, dl + p, v);
            p += v;
            o += v;
        }
    }
    return o;
}

static size_t gen_text(unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char)('a' + (rndn(6) * 3 + i / 37) % 26);
    return n;
}

int main(void) {
    enum { N = 6000 };
    static unsigned char old[N], nw[N + 400], out[N + 400];
    size_t no = gen_text(old, N);
    /* weak checksum sanity: rolling equals recomputation */
    uint32_t w = weak_sum(old, BS);
    for (size_t i = 0; i + BS < 300; i++) {
        w = weak_roll(w, old[i], old[i + BS], BS);
        check(w == weak_sum(old + i + 1, BS), "rolling checksum");
    }
    printf("%-22s %6s %6s %7s %7s\n", "scenario", "copies", "lits", "litbytes", "delta");
    for (int sc = 0; sc < 6; sc++) {
        size_t nn = 0;
        const char *name = "";
        switch (sc) {
        case 0:
            name = "identical";
            memcpy(nw, old, no);
            nn = no;
            break;
        case 1:
            name = "insert 13 bytes";
            memcpy(nw, old, 1000);
            memcpy(nw + 1000, "INSERTED-TEXT", 13);
            memcpy(nw + 1013, old + 1000, no - 1000);
            nn = no + 13;
            break;
        case 2:
            name = "delete 200 bytes";
            memcpy(nw, old, 2500);
            memcpy(nw + 2500, old + 2700, no - 2700);
            nn = no - 200;
            break;
        case 3:
            name = "scattered byte edits";
            memcpy(nw, old, no);
            for (int i = 0; i < 12; i++)
                nw[rndn(N)] ^= 0x55;
            nn = no;
            break;
        case 4:
            name = "append tail";
            memcpy(nw, old, no);
            for (int i = 0; i < 300; i++)
                nw[no + (size_t)i] = (unsigned char)('A' + i % 26);
            nn = no + 300;
            break;
        default:
            name = "unrelated";
            nn = gen_text(nw, N);
            for (size_t i = 0; i < nn; i++)
                nw[i] ^= 0x80;
        }
        Delta d;
        make_delta(old, no, nw, nn, &d);
        spit("patch.delta", d.buf, d.len); /* round trip through a file */
        size_t dn;
        unsigned char *dl = slurp("patch.delta", &dn);
        size_t got = apply_delta(old, no, dl, dn, out);
        check(got == nn && memcmp(out, nw, nn) == 0, "patched equals new");
        printf("%-22s %6ld %6ld %7ld %7zu\n", name, d.copies, d.literals, d.lit_bytes, d.len);
        free(dl);
        free(d.buf);
    }
    unlink("patch.delta");
    return 0;
}
