/*
 * title: Free-space bitmap allocator for a file-backed volume with extent lists
 * topic: io_files
 * covers: on-disk bitmap, first-fit run search, extent lists, fragmentation, defragmentation by rewrite, reopen persistence
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
enum { BSZ = 32, NBLK = 192, DATA0 = 2, MAXEXT = 12, MAXFILES = 40 };

typedef struct {
    int used;
    uint32_t size; /* bytes */
    int next;
    struct { uint16_t start, len; } ext[MAXEXT];
} File;

typedef struct {
    int fd;
    unsigned char bitmap[NBLK / 8];
    File files[MAXFILES];
    long bitmap_writes;
} Vol;

static int bit_get(const Vol *v, int b) { return (v->bitmap[b >> 3] >> (b & 7)) & 1; }
static void bit_set(Vol *v, int b, int on) {
    if (on)
        v->bitmap[b >> 3] |= (unsigned char)(1u << (b & 7));
    else
        v->bitmap[b >> 3] &= (unsigned char)~(1u << (b & 7));
}
/* Persist the bitmap into block 1 area (NBLK/8 = 24 bytes fits one block). */
static void bitmap_flush(Vol *v) {
    pwrite_all(v->fd, v->bitmap, sizeof v->bitmap, BSZ);
    v->bitmap_writes++;
}

static void vol_format(Vol *v) {
    memset(v, 0, sizeof *v);
    v->fd = open("vol.img", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(v->fd >= 0, "vol open");
    unsigned char zero[BSZ * NBLK];
    memset(zero, 0, sizeof zero);
    pwrite_all(v->fd, zero, sizeof zero, 0);
    for (int b = 0; b < DATA0; b++)
        bit_set(v, b, 1);
    bitmap_flush(v);
}

/* Find the first free run of exactly `want` blocks, or the longest run below `want`. */
static int find_run(const Vol *v, int want, int *len_out) {
    int best_start = -1, best_len = 0;
    for (int b = DATA0; b < NBLK;) {
        if (bit_get(v, b)) {
            b++;
            continue;
        }
        int e = b;
        while (e < NBLK && !bit_get(v, e))
            e++;
        int rl = e - b;
        if (rl >= want) {
            *len_out = want;
            return b;
        }
        if (rl > best_len) {
            best_len = rl;
            best_start = b;
        }
        b = e;
    }
    *len_out = best_len;
    return best_start;
}

static int free_blocks(const Vol *v) {
    int n = 0;
    for (int b = DATA0; b < NBLK; b++)
        n += !bit_get(v, b);
    return n;
}

static void fill_data(unsigned char *p, int id, uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        p[i] = (unsigned char)(id * 37u + (off + i) * 5u);
}

static int create(Vol *v, int id, uint32_t size) {
    int need = (int)((size + BSZ - 1) / BSZ);
    if (need > free_blocks(v))
        return 0;
    File *f = &v->files[id];
    memset(f, 0, sizeof *f);
    f->used = 1;
    f->size = size;
    int left = need;
    uint32_t off = 0;
    while (left > 0) {
        int len;
        int start = find_run(v, left, &len);
        check(start >= 0 && len > 0 && f->next < MAXEXT, "extent allocation");
        for (int b = start; b < start + len; b++)
            bit_set(v, b, 1);
        f->ext[f->next].start = (uint16_t)start;
        f->ext[f->next].len = (uint16_t)len;
        f->next++;
        for (int b = start; b < start + len; b++) {
            unsigned char blk[BSZ];
            uint32_t n = size - off < BSZ ? size - off : BSZ;
            memset(blk, 0, BSZ);
            fill_data(blk, id, off, n);
            pwrite_all(v->fd, blk, BSZ, (off_t)b * BSZ);
            off += n;
        }
        left -= len;
    }
    bitmap_flush(v);
    return 1;
}

static void delete_file(Vol *v, int id) {
    File *f = &v->files[id];
    for (int i = 0; i < f->next; i++)
        for (int b = f->ext[i].start; b < f->ext[i].start + f->ext[i].len; b++)
            bit_set(v, b, 0);
    memset(f, 0, sizeof *f);
    bitmap_flush(v);
}

static void verify(const Vol *v, int id) {
    const File *f = &v->files[id];
    uint32_t off = 0;
    for (int i = 0; i < f->next; i++)
        for (int b = f->ext[i].start; b < f->ext[i].start + f->ext[i].len; b++) {
            unsigned char blk[BSZ], want[BSZ];
            check(pread_upto(v->fd, blk, BSZ, (off_t)b * BSZ) == BSZ, "read block");
            uint32_t n = f->size - off < BSZ ? f->size - off : BSZ;
            fill_data(want, id, off, n);
            check(memcmp(blk, want, n) == 0, "file data intact");
            check(bit_get(v, b), "block marked used");
            off += n;
        }
    check(off == f->size, "extent coverage");
}

static void picture(const Vol *v, const char *label) {
    char line[NBLK / 4 + 1];
    for (int i = 0; i < NBLK / 4; i++) { /* each char summarises 4 blocks: '.' free, '#' full, '+' mixed */
        int u = 0;
        for (int k = 0; k < 4; k++)
            u += bit_get(v, i * 4 + k);
        line[i] = u == 0 ? '.' : (u == 4 ? '#' : '+');
    }
    line[NBLK / 4] = 0;
    int largest = 0, runs = 0, cur = 0;
    for (int b = DATA0; b <= NBLK; b++) {
        if (b < NBLK && !bit_get(v, b))
            cur++;
        else {
            if (cur) {
                runs++;
                if (cur > largest)
                    largest = cur;
            }
            cur = 0;
        }
    }
    int nfiles = 0, nexts = 0;
    for (int i = 0; i < MAXFILES; i++)
        if (v->files[i].used) {
            nfiles++;
            nexts += v->files[i].next;
        }
    printf("%-12s |%s| free=%d runs=%d largest=%d files=%d extents=%d\n", label, line, free_blocks(v), runs, largest,
           nfiles, nexts);
}

int main(void) {
    Vol v;
    vol_format(&v);
    int made = 0;
    for (int id = 0; id < 30; id++) {
        uint32_t size = 20 + rndn(200);
        made += create(&v, id, size);
    }
    picture(&v, "filled");
    for (int id = 0; id < 30; id++)
        if (v.files[id].used)
            verify(&v, id);
    for (int id = 1; id < 30; id += 2)
        if (v.files[id].used)
            delete_file(&v, id);
    picture(&v, "fragmented");
    int extra = 0;
    for (int id = 30; id < MAXFILES; id++)
        extra += create(&v, id, 300 + rndn(300));
    picture(&v, "refilled");
    for (int id = 0; id < MAXFILES; id++)
        if (v.files[id].used)
            verify(&v, id);

    /* Bitmap persistence: reload from the file and compare. */
    unsigned char disk[NBLK / 8];
    check(pread_upto(v.fd, disk, sizeof disk, BSZ) == sizeof disk, "bitmap read");
    check(memcmp(disk, v.bitmap, sizeof disk) == 0, "bitmap on disk equals memory");

    /* Defragment: read all files, free everything, re-create in id order (contiguous when possible). */
    uint32_t sizes[MAXFILES];
    for (int id = 0; id < MAXFILES; id++) {
        sizes[id] = v.files[id].used ? v.files[id].size : 0;
        if (v.files[id].used)
            delete_file(&v, id);
    }
    check(free_blocks(&v) == NBLK - DATA0, "all blocks freed");
    for (int id = 0; id < MAXFILES; id++)
        if (sizes[id])
            check(create(&v, id, sizes[id]), "recreate");
    picture(&v, "defragmented");
    for (int id = 0; id < MAXFILES; id++)
        if (v.files[id].used)
            verify(&v, id);
    int max_ext = 0;
    for (int id = 0; id < MAXFILES; id++)
        if (v.files[id].next > max_ext)
            max_ext = v.files[id].next;
    printf("created=%d refilled=%d max extents per file after defrag=%d bitmap writes=%ld\n", made, extra, max_ext,
           v.bitmap_writes);
    close(v.fd);
    unlink("vol.img");
    return 0;
}
