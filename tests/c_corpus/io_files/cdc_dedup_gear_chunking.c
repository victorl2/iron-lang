/*
 * title: Content-defined chunking dedup store with gear rolling hash
 * topic: io_files
 * covers: rolling hash, content-defined chunk boundaries, chunk store file, recipes, fixed-size vs CDC dedup, byte-exact restore
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
enum { MINC = 48, MAXC = 1024, MASK = 0xFF, FIXED = 256, MAXCH = 512 };

static uint32_t gear[256];

static void init_gear(void) {
    for (int i = 0; i < 256; i++)
        gear[i] = (uint32_t)rnd();
}

/* Length of the next content-defined chunk starting at p. */
static size_t cdc_cut(const unsigned char *p, size_t n) {
    if (n <= MINC)
        return n;
    uint32_t h = 0;
    size_t lim = n < MAXC ? n : MAXC;
    for (size_t i = 0; i < lim; i++) {
        h = (h << 1) + gear[p[i]];
        if (i + 1 >= MINC && (h & MASK) == 0)
            return i + 1;
    }
    return lim;
}

static uint64_t fnv64(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

typedef struct {
    uint64_t id;
    uint32_t off, len;
} Entry;

typedef struct {
    int fd;
    uint32_t size;
    Entry ent[4 * MAXCH];
    int n;
    long stored_bytes, dup_bytes, dup_chunks;
} Store;

typedef struct {
    int idx[4 * MAXCH]; /* entry index per chunk */
    int n;
} Recipe;

static void store_init(Store *s, const char *path) {
    memset(s, 0, sizeof *s);
    s->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(s->fd >= 0, "store open");
}

static int store_put(Store *s, const unsigned char *p, size_t len) {
    uint64_t id = fnv64(p, len) ^ len;
    for (int i = 0; i < s->n; i++)
        if (s->ent[i].id == id && s->ent[i].len == len) { /* dedup hit (verify bytes) */
            unsigned char tmp[MAXC];
            check(pread_upto(s->fd, tmp, len, s->ent[i].off) == len && memcmp(tmp, p, len) == 0, "no collision");
            s->dup_bytes += (long)len;
            s->dup_chunks++;
            return i;
        }
    check(s->n < 4 * MAXCH, "entries");
    pwrite_all(s->fd, p, len, s->size);
    s->ent[s->n].id = id;
    s->ent[s->n].off = s->size;
    s->ent[s->n].len = (uint32_t)len;
    s->size += (uint32_t)len;
    s->stored_bytes += (long)len;
    return s->n++;
}

static void ingest(Store *s, Recipe *r, const unsigned char *data, size_t n, int fixed) {
    size_t pos = 0;
    r->n = 0;
    while (pos < n) {
        size_t c = fixed ? (n - pos < FIXED ? n - pos : FIXED) : cdc_cut(data + pos, n - pos);
        r->idx[r->n++] = store_put(s, data + pos, c);
        pos += c;
    }
}

static size_t restore(Store *s, const Recipe *r, unsigned char *out) {
    size_t pos = 0;
    for (int i = 0; i < r->n; i++) {
        Entry *e = &s->ent[r->idx[i]];
        check(pread_upto(s->fd, out + pos, e->len, e->off) == e->len, "restore read");
        pos += e->len;
    }
    return pos;
}

int main(void) {
    init_gear();
    enum { N = 30000 };
    static unsigned char v1[N + 64], v2[N + 200], v3[N + 200], back[N + 300];
    /* v1: text-like data with repeated phrases so chunks recur; v2: insert 37 bytes near the front and change
     * a few bytes later; v3: v2 with a 500 byte block deleted from the middle. */
    static const char *words[8] = {"alpha ", "beta ", "gamma ", "delta ", "epsilon ", "zeta ", "eta ", "theta "};
    size_t n1 = 0;
    while (n1 < N) {
        const char *w = words[rndn(8)];
        size_t l = strlen(w);
        if (n1 + l > N)
            break;
        memcpy(v1 + n1, w, l);
        n1 += l;
        if (rndn(4) == 0)
            v1[n1++] = (unsigned char)('0' + rndn(10));
    }
    size_t n2 = 0;
    memcpy(v2, v1, 700);
    n2 = 700;
    for (int i = 0; i < 37; i++)
        v2[n2++] = (unsigned char)('#' + (i % 5));
    memcpy(v2 + n2, v1 + 700, n1 - 700);
    n2 += n1 - 700;
    v2[n2 - 5000] ^= 0x20;
    v2[n2 - 9000] ^= 0x20;
    size_t n3 = 0;
    memcpy(v3, v2, 12000);
    n3 = 12000;
    memcpy(v3 + n3, v2 + 12500, n2 - 12500);
    n3 += n2 - 12500;
    printf("versions: %zu, %zu, %zu bytes\n", n1, n2, n3);

    for (int fixed = 1; fixed >= 0; fixed--) {
        Store s;
        store_init(&s, fixed ? "fixed.store" : "cdc.store");
        static Recipe rc[3];
        const unsigned char *vers[3] = {v1, v2, v3};
        size_t lens[3] = {n1, n2, n3};
        printf("%s chunking:\n", fixed ? "fixed-size" : "content-defined");
        long total_in = 0;
        for (int v = 0; v < 3; v++) {
            long before = s.stored_bytes;
            ingest(&s, &rc[v], vers[v], lens[v], fixed);
            total_in += (long)lens[v];
            printf("  v%d: chunks=%d new bytes stored=%ld\n", v + 1, rc[v].n, s.stored_bytes - before);
        }
        for (int v = 0; v < 3; v++) {
            size_t got = restore(&s, &rc[v], back);
            check(got == lens[v] && memcmp(back, vers[v], got) == 0, "restore bytes");
        }
        printf("  input=%ld stored=%ld dedup ratio=%.2f duplicate chunks=%ld\n", total_in, s.stored_bytes,
               (double)total_in / (double)s.stored_bytes, s.dup_chunks);
        if (!fixed) {
            int minc = 1 << 30, maxc = 0;
            for (int i = 0; i < s.n; i++) {
                if ((int)s.ent[i].len < minc) minc = (int)s.ent[i].len;
                if ((int)s.ent[i].len > maxc) maxc = (int)s.ent[i].len;
            }
            printf("  unique chunks=%d size range %d..%d\n", s.n, minc, maxc);
        }
        close(s.fd);
        unlink(fixed ? "fixed.store" : "cdc.store");
    }
    return 0;
}
