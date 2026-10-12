/*
 * title: Read-only mmap reader for a large sorted record file with bounds checks
 * topic: io_files
 * covers: mmap PROT_READ, offset index binary search, zero-copy views, prefix range scan, bounds-checked slices, corrupt-file rejection
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

#include <sys/mman.h>

/* Header (16): "MMR1" | count u32 | index_off u32 | data_end u32.
 * Record: klen u8 | vlen u16 | key | value.  Index: count x u32 record offsets, sorted by key. */
typedef struct {
    const unsigned char *base;
    size_t len;
    uint32_t count, index_off;
    long probes;
} View;

static const unsigned char *slice(const View *v, size_t off, size_t n) {
    if (off > v->len || n > v->len - off)
        return NULL;
    return v->base + off;
}

static int view_open(View *v, const unsigned char *base, size_t len) {
    memset(v, 0, sizeof *v);
    v->base = base;
    v->len = len;
    const unsigned char *h = slice(v, 0, 16);
    if (!h || memcmp(h, "MMR1", 4) != 0)
        return 0;
    v->count = get32(h + 4);
    v->index_off = get32(h + 8);
    if (!slice(v, v->index_off, (size_t)v->count * 4))
        return 0;
    return 1;
}

/* Returns 1 and pointers into the mapping for record i; 0 if the record is malformed. */
static int rec_at(const View *v, uint32_t i, const unsigned char **key, unsigned *klen, const unsigned char **val,
                  unsigned *vlen) {
    const unsigned char *ix = slice(v, v->index_off + (size_t)i * 4, 4);
    if (!ix)
        return 0;
    size_t off = get32(ix);
    const unsigned char *r = slice(v, off, 3);
    if (!r)
        return 0;
    *klen = r[0];
    *vlen = get16(r + 1);
    *key = slice(v, off + 3, *klen);
    *val = slice(v, off + 3 + *klen, *vlen);
    return *key && *val;
}

static int cmp_key(const unsigned char *a, unsigned al, const char *b, unsigned bl) {
    unsigned m = al < bl ? al : bl;
    int c = memcmp(a, b, m);
    if (c)
        return c;
    return al < bl ? -1 : (al > bl);
}

/* Lower bound: first record index whose key >= target. */
static uint32_t lower_bound(View *v, const char *target) {
    uint32_t lo = 0, hi = v->count;
    unsigned tl = (unsigned)strlen(target);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const unsigned char *k, *val;
        unsigned kl, vl;
        check(rec_at(v, mid, &k, &kl, &val, &vl), "rec_at in lower_bound");
        v->probes++;
        if (cmp_key(k, kl, target, tl) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void) {
    enum { N = 4000 };
    static char *keys[N];
    /* keys: two-letter prefix + 5 digits, unique via the counter */
    for (int i = 0; i < N; i++) {
        char k[24];
        int c1 = (int)rndn(6), c2 = (int)rndn(4), c3 = (int)rndn(7);
        snprintf(k, sizeof k, "%c%c-%05d", 'a' + c1, 'a' + c2, i * 7 + c3);
        keys[i] = strdup(k);
    }
    qsort(keys, N, sizeof keys[0], cmp_str);
    size_t cap = 16 + (size_t)N * (3 + 8 + 40 + 4) + 64;
    unsigned char *img = calloc(cap, 1);
    check(img != NULL, "image");
    size_t pos = 16;
    static uint32_t offs[N];
    for (int i = 0; i < N; i++) {
        unsigned kl = (unsigned)strlen(keys[i]);
        unsigned vl = 4 + rndn(36);
        offs[i] = (uint32_t)pos;
        img[pos] = (unsigned char)kl;
        put16(img + pos + 1, vl);
        memcpy(img + pos + 3, keys[i], kl);
        for (unsigned j = 0; j < vl; j++)
            img[pos + 3 + kl + j] = (unsigned char)(i * 3u + j);
        pos += 3 + kl + vl;
    }
    uint32_t index_off = (uint32_t)pos;
    for (int i = 0; i < N; i++)
        put32(img + pos + 4 * (size_t)i, offs[i]);
    pos += 4 * (size_t)N;
    memcpy(img, "MMR1", 4);
    put32(img + 4, N);
    put32(img + 8, index_off);
    put32(img + 12, index_off);
    spit("big.mmr", img, pos);

    int fd = open("big.mmr", O_RDONLY);
    check(fd >= 0, "open");
    struct stat st;
    check(fstat(fd, &st) == 0, "fstat");
    size_t len = (size_t)st.st_size;
    void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "mmap");
    close(fd); /* the mapping stays valid */
    View v;
    check(view_open(&v, (const unsigned char *)map, len), "view open");
    printf("file bytes=%zu records=%u index at %u\n", len, v.count, v.index_off);

    /* Every key found by binary search; value bytes match the generator. */
    for (int i = 0; i < N; i++) {
        uint32_t at = lower_bound(&v, keys[i]);
        check(at == (uint32_t)i, "exact lower bound");
        const unsigned char *k, *val;
        unsigned kl, vl;
        check(rec_at(&v, at, &k, &kl, &val, &vl), "rec");
        check(kl == strlen(keys[i]) && memcmp(k, keys[i], kl) == 0, "key bytes");
        for (unsigned j = 0; j < vl; j++)
            check(val[j] == (unsigned char)((unsigned)i * 3u + j), "value bytes");
    }
    printf("average probes per lookup=%.2f\n", (double)v.probes / N);

    /* Prefix range scan: count keys starting with "ca-" and "dd". */
    static const char *prefixes[3] = {"ca-", "dd", "zz"};
    for (int p = 0; p < 3; p++) {
        uint32_t lo = lower_bound(&v, prefixes[p]);
        uint32_t n = 0;
        for (uint32_t i = lo; i < v.count; i++) {
            const unsigned char *k, *val;
            unsigned kl, vl;
            check(rec_at(&v, i, &k, &kl, &val, &vl), "scan rec");
            if (kl < strlen(prefixes[p]) || memcmp(k, prefixes[p], strlen(prefixes[p])) != 0)
                break;
            n++;
        }
        int brute = 0;
        for (int i = 0; i < N; i++)
            brute += strncmp(keys[i], prefixes[p], strlen(prefixes[p])) == 0;
        check((int)n == brute, "prefix count");
        printf("prefix \"%s\": %u keys\n", prefixes[p], n);
    }
    /* Whole-file checksum through the mapping equals a pread-based one. */
    uint32_t c1 = crc32_update(0, v.base, v.len);
    size_t sn;
    unsigned char *all = slurp("big.mmr", &sn);
    check(c1 == crc32_update(0, all, sn) && sn == v.len, "mapped view equals file contents");
    free(all);
    printf("whole-file crc=%08x\n", c1);
    munmap(map, len);

    /* Corrupt copies must be rejected by the bounds checks, never crash. */
    int rejected = 0, accepted = 0;
    for (int variant = 0; variant < 4; variant++) {
        unsigned char *bad = malloc(pos);
        memcpy(bad, img, pos);
        size_t blen = pos;
        if (variant == 0)
            put32(bad + 8, (uint32_t)pos + 100); /* index beyond EOF */
        else if (variant == 1)
            put32(bad + 4, 0x7FFFFFFFu); /* absurd count */
        else if (variant == 2)
            blen = pos / 2; /* truncated file */
        else
            put32(bad + index_off + 4 * 10, (uint32_t)pos - 2); /* one record offset near EOF */
        View bv;
        int ok = view_open(&bv, bad, blen);
        if (ok && variant == 3) {
            const unsigned char *k, *val;
            unsigned kl, vl;
            ok = rec_at(&bv, 10, &k, &kl, &val, &vl);
        }
        if (ok)
            accepted++;
        else
            rejected++;
        free(bad);
    }
    check(rejected == 4 && accepted == 0, "corrupt views rejected");
    printf("corrupt variants rejected=%d accepted=%d\n", rejected, accepted);
    for (int i = 0; i < N; i++)
        free(keys[i]);
    free(img);
    unlink("big.mmr");
    return 0;
}
