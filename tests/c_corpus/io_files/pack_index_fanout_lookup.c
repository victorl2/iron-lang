/*
 * title: Pack file with a git-style fanout index and large-offset table
 * topic: io_files
 * covers: pack and index files, 256-entry fanout table, binary search within a fanout bucket, crc per entry, large-offset escape table, lookup accounting vs linear scan
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

/* Pack: "PACK" | count u32 | entries: id u32 | len u16 | data ...
 * Index: "IDX2" | fanout[256] u32 (cumulative counts by top id byte) | ids[n] u32 sorted | crcs[n] u32 |
 *        offsets[n] u32 (MSB set => index into the large-offset table) | large[] u64 (stored as two u32). */
enum { N = 3000, LARGE_THRESHOLD = 20000 };

typedef struct {
    uint32_t id;
    uint32_t off;
    uint32_t crc;
    unsigned len;
} Ent;

static int cmp_ent(const void *a, const void *b) {
    uint32_t x = ((const Ent *)a)->id, y = ((const Ent *)b)->id;
    return x < y ? -1 : (x > y);
}

typedef struct {
    unsigned char *idx;
    size_t idx_len;
    uint32_t n;
    int pack_fd;
    long probes;
} Pack;

static void pack_open(Pack *p) {
    p->idx = slurp("objs.idx", &p->idx_len);
    check(!memcmp(p->idx, "IDX2", 4), "idx magic");
    p->n = get32(p->idx + 4 + 255 * 4);
    p->pack_fd = open("objs.pack", O_RDONLY);
    check(p->pack_fd >= 0, "open pack");
    p->probes = 0;
}
static void pack_close(Pack *p) {
    free(p->idx);
    close(p->pack_fd);
}

static const unsigned char *ids_tab(const Pack *p) { return p->idx + 4 + 256 * 4; }
static const unsigned char *crc_tab(const Pack *p) { return ids_tab(p) + (size_t)p->n * 4; }
static const unsigned char *off_tab(const Pack *p) { return crc_tab(p) + (size_t)p->n * 4; }
static const unsigned char *large_tab(const Pack *p) { return off_tab(p) + (size_t)p->n * 4; }

/* Returns 1 and fills data; verifies crc. */
static int pack_get(Pack *p, uint32_t id, unsigned char *out, unsigned *len) {
    unsigned top = id >> 24;
    uint32_t lo = top ? get32(p->idx + 4 + (top - 1) * 4) : 0, hi = get32(p->idx + 4 + top * 4);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        p->probes++;
        uint32_t v = get32(ids_tab(p) + (size_t)mid * 4);
        if (v == id) {
            uint32_t o = get32(off_tab(p) + (size_t)mid * 4);
            uint64_t off;
            if (o & 0x80000000u) {
                const unsigned char *L = large_tab(p) + (size_t)(o & 0x7FFFFFFFu) * 8;
                off = (uint64_t)get32(L) | ((uint64_t)get32(L + 4) << 32);
            } else
                off = o;
            unsigned char hdr[6];
            check(pread_upto(p->pack_fd, hdr, 6, (off_t)off) == 6, "entry header");
            check(get32(hdr) == id, "entry id");
            *len = get16(hdr + 4);
            check(pread_upto(p->pack_fd, out, *len, (off_t)off + 6) == *len, "entry data");
            check(crc32_update(0, out, *len) == get32(crc_tab(p) + (size_t)mid * 4), "entry crc");
            return 1;
        }
        if (v < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

int main(void) {
    static Ent ent[N];
    int pfd = open("objs.pack", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(pfd >= 0, "pack create");
    unsigned char hdr[8];
    memcpy(hdr, "PACK", 4);
    put32(hdr + 4, N);
    write_all(pfd, hdr, 8);
    uint32_t off = 8;
    int nid = 0;
    static uint32_t used[N];
    while (nid < N) {
        uint32_t id = (uint32_t)rnd() | 1u;
        if (nid % 40 == 0)
            id = (id & 0x00FFFFFFu) | (rndn(4) << 24); /* cluster some ids into a few fanout buckets */
        int dup = 0;
        for (int i = 0; i < nid && !dup; i++)
            dup = used[i] == id;
        if (dup)
            continue;
        used[nid] = id;
        unsigned len = 8 + rndn(24);
        unsigned char rec[6 + 40];
        put32(rec, id);
        put16(rec + 4, len);
        for (unsigned i = 0; i < len; i++)
            rec[6 + i] = (unsigned char)(id * 31u + i);
        write_all(pfd, rec, 6 + len);
        ent[nid].id = id;
        ent[nid].off = off;
        ent[nid].crc = crc32_update(0, rec + 6, len);
        ent[nid].len = len;
        off += 6 + len;
        nid++;
    }
    close(pfd);
    static Ent sorted[N];
    memcpy(sorted, ent, sizeof ent);
    qsort(sorted, N, sizeof sorted[0], cmp_ent);

    /* Build the index. */
    size_t large_count = 0;
    for (int i = 0; i < N; i++)
        large_count += sorted[i].off >= LARGE_THRESHOLD;
    size_t isz = 4 + 256 * 4 + (size_t)N * 12 + large_count * 8;
    unsigned char *idx = calloc(1, isz);
    check(idx != NULL, "idx alloc");
    memcpy(idx, "IDX2", 4);
    uint32_t fan[256] = {0};
    for (int i = 0; i < N; i++)
        fan[sorted[i].id >> 24]++;
    uint32_t run = 0;
    for (int b = 0; b < 256; b++) {
        run += fan[b];
        put32(idx + 4 + 4 * b, run);
    }
    unsigned char *ids = idx + 4 + 256 * 4, *crcs = ids + (size_t)N * 4, *offs = crcs + (size_t)N * 4,
                  *large = offs + (size_t)N * 4;
    size_t li = 0;
    for (int i = 0; i < N; i++) {
        put32(ids + 4 * i, sorted[i].id);
        put32(crcs + 4 * i, sorted[i].crc);
        if (sorted[i].off >= LARGE_THRESHOLD) {
            put32(offs + 4 * i, 0x80000000u | (uint32_t)li);
            put32(large + 8 * li, sorted[i].off);
            put32(large + 8 * li + 4, 0);
            li++;
        } else
            put32(offs + 4 * i, sorted[i].off);
    }
    spit("objs.idx", idx, isz);
    free(idx);
    printf("objects=%d pack bytes=%ld index bytes=%zu large-offset entries=%zu\n", N, file_size("objs.pack"), isz,
           large_count);

    Pack pk;
    pack_open(&pk);
    check(pk.n == N, "index count");
    /* Every stored id resolves with correct data. */
    long probes_hit = 0;
    for (int i = 0; i < N; i++) {
        unsigned char buf[64];
        unsigned len;
        check(pack_get(&pk, ent[i].id, buf, &len) && len == ent[i].len, "hit");
        for (unsigned k = 0; k < len; k++)
            check(buf[k] == (unsigned char)(ent[i].id * 31u + k), "entry bytes");
    }
    probes_hit = pk.probes;
    /* Misses: ids not in the pack (even ids; stored ids are odd). */
    pk.probes = 0;
    int misses = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t id = ((uint32_t)rnd() & ~1u);
        unsigned char buf[64];
        unsigned len;
        check(!pack_get(&pk, id, buf, &len), "miss");
        misses++;
    }
    /* Fanout narrowing: compare against an un-fanned binary search over all ids. */
    long full_probes = 0;
    for (int i = 0; i < N; i++) {
        uint32_t lo = 0, hi = N;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            full_probes++;
            uint32_t v = get32(ids_tab(&pk) + (size_t)mid * 4);
            if (v == ent[i].id)
                break;
            if (v < ent[i].id)
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    int big = 0;
    for (int i = 0; i < N; i++)
        big += ent[i].off >= LARGE_THRESHOLD;
    printf("hits verified=%d (crc checked), misses=%d\n", N, misses);
    printf("avg probes with fanout=%.2f, without fanout=%.2f, misses=%.2f\n", (double)probes_hit / N,
           (double)full_probes / N, (double)pk.probes / misses);
    printf("entries resolved through the large-offset table=%d\n", big);
    pack_close(&pk);
    unlink("objs.pack");
    unlink("objs.idx");
    return 0;
}
