/*
 * title: Doublewrite buffer that repairs torn page writes
 * topic: io_files
 * covers: torn page detection by checksum, doublewrite area with batch header, all-or-nothing page batches, recovery by copy-back, comparison with unprotected writes
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

enum { PSZ = 64, NPG = 16, BATCH = 4 };
/* Data page: crc u32 | page number u32 | version u32 | payload 52 bytes.  crc covers bytes 4..63. */
/* Doublewrite file: "DWB1" | count u32 | crc32 of all following bytes | count x (page no u32 + PSZ bytes). */

static void make_page(unsigned char *pg, int no, uint32_t ver) {
    memset(pg, 0, PSZ);
    put32(pg + 4, (uint32_t)no);
    put32(pg + 8, ver);
    for (int i = 12; i < PSZ; i++)
        pg[i] = (unsigned char)(no * 29 + (int)ver * 17 + i);
    put32(pg, crc32_update(0, pg + 4, PSZ - 4));
}
static int page_ok(const unsigned char *pg, int no) {
    return get32(pg) == crc32_update(0, pg + 4, PSZ - 4) && get32(pg + 4) == (uint32_t)no;
}

static int fd_data;

static void init_db(void) {
    for (int p = 0; p < NPG; p++) {
        unsigned char pg[PSZ];
        make_page(pg, p, 1);
        pwrite_all(fd_data, pg, PSZ, (off_t)p * PSZ);
    }
}

/* Flush a batch: with doublewrite, first the whole batch into dw.buf, then to home locations.
 * `torn_page`/`torn_bytes` describe a crash: home write of batch element `torn_page` completes only
 * `torn_bytes` bytes and nothing after runs.  crash_in_dw >= 0 tears the dw file after that many bytes. */
static void flush_batch(const int *pages, int n, uint32_t ver, int use_dw, int torn_page, int torn_bytes,
                        long crash_in_dw) {
    unsigned char img[BATCH][PSZ];
    for (int i = 0; i < n; i++)
        make_page(img[i], pages[i], ver);
    if (use_dw) {
        unsigned char dw[12 + BATCH * (4 + PSZ)];
        size_t len = 12;
        for (int i = 0; i < n; i++) {
            put32(dw + len, (uint32_t)pages[i]);
            memcpy(dw + len + 4, img[i], PSZ);
            len += 4 + PSZ;
        }
        memcpy(dw, "DWB1", 4);
        put32(dw + 4, (uint32_t)n);
        put32(dw + 8, crc32_update(0, dw + 12, len - 12));
        size_t w = crash_in_dw >= 0 && (size_t)crash_in_dw < len ? (size_t)crash_in_dw : len;
        spit("dw.buf", dw, w);
        if (w < len)
            return; /* crashed while writing the doublewrite area: home pages untouched */
    }
    for (int i = 0; i < n; i++) {
        if (torn_page >= 0 && i > torn_page)
            return;
        size_t w = (i == torn_page) ? (size_t)torn_bytes : PSZ;
        pwrite_all(fd_data, img[i], w, (off_t)pages[i] * PSZ);
        if (i == torn_page)
            return;
    }
    if (use_dw)
        unlink("dw.buf"); /* batch fully checkpointed */
}

/* Recovery: if dw.buf holds a complete, checksummed batch, copy its pages over the home locations. */
static int recover(void) {
    if (file_size("dw.buf") < 0)
        return 0;
    size_t n;
    unsigned char *dw = slurp("dw.buf", &n);
    int restored = 0;
    if (n >= 12 && !memcmp(dw, "DWB1", 4)) {
        uint32_t cnt = get32(dw + 4);
        if (n == 12 + (size_t)cnt * (4 + PSZ) && get32(dw + 8) == crc32_update(0, dw + 12, n - 12)) {
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t no = get32(dw + 12 + i * (4 + PSZ));
                pwrite_all(fd_data, dw + 16 + i * (4 + PSZ), PSZ, (off_t)no * PSZ);
                restored++;
            }
        }
    }
    free(dw);
    unlink("dw.buf");
    return restored;
}

static uint32_t page_version(int no, int *ok) {
    unsigned char pg[PSZ];
    check(pread_upto(fd_data, pg, PSZ, (off_t)no * PSZ) == PSZ, "read data page");
    *ok = page_ok(pg, no);
    return get32(pg + 8);
}

int main(void) {
    fd_data = open("data.pg", O_RDWR | O_CREAT | O_TRUNC, 0644);
    check(fd_data >= 0, "open data");
    int pages[BATCH] = {3, 7, 8, 12};
    int scenarios = 0;
    int corrupt_plain = 0, corrupt_dw = 0, old_state = 0, new_state = 0, restored_total = 0;
    for (int use_dw = 0; use_dw < 2; use_dw++) {
        /* torn home write: element t (0..BATCH-1), torn after b bytes; and "no tear" as control */
        for (int t = 0; t < BATCH; t++)
            for (int b = 0; b <= PSZ; b += 7) {
                init_db();
                flush_batch(pages, BATCH, 2, use_dw, t, b, -1);
                int r = use_dw ? recover() : 0;
                restored_total += r;
                int nnew = 0, nold = 0, bad = 0;
                for (int i = 0; i < BATCH; i++) {
                    int ok;
                    uint32_t v = page_version(pages[i], &ok);
                    if (!ok)
                        bad++;
                    else if (v == 2)
                        nnew++;
                    else
                        nold++;
                }
                if (use_dw) {
                    check(bad == 0, "no corrupt page after doublewrite recovery");
                    check(nnew == BATCH, "batch applied completely after recovery");
                    corrupt_dw += bad;
                    new_state++;
                } else
                    corrupt_plain += bad > 0;
                scenarios++;
                (void)nold;
            }
        /* crash while writing the doublewrite area itself */
        if (use_dw)
            for (long c = 0; c < 12 + BATCH * (4 + PSZ); c += 13) {
                init_db();
                flush_batch(pages, BATCH, 2, 1, -1, 0, c);
                int r = recover();
                check(r == 0, "torn dw is ignored");
                for (int i = 0; i < NPG; i++) {
                    int ok;
                    check(page_version(i, &ok) == 1 && ok, "database still at the old version");
                }
                old_state++;
                scenarios++;
            }
    }
    printf("scenarios=%d\n", scenarios);
    printf("without doublewrite: torn scenarios leaving a corrupt page=%d\n", corrupt_plain);
    printf("with doublewrite: corrupt pages=%d, batches completed by recovery=%d, torn dw discarded=%d\n", corrupt_dw,
           new_state, old_state);
    printf("pages restored from dw=%d\n", restored_total);
    close(fd_data);
    unlink("data.pg");
    return 0;
}
