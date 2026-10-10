/*
 * title: Memory-mapped append log with checksums and crash recovery
 * topic: memory
 * covers: mmap log, length-prefixed CRC records, commit pointer ordering, torn-write recovery, remap on growth
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG_MAGIC 0x4C4F4731u
#define HDR 16u

/* file layout: [magic u32][committed u32][count u32][pad u32] then records:
   [len u32][crc u32][payload len bytes], each padded to 8 */

typedef struct {
    int fd;
    unsigned char *m;
    size_t size;
} Log;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t crc32(const unsigned char *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t rd32(const unsigned char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(unsigned char *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static size_t rec_span(uint32_t len) {
    return (8u + len + 7u) & ~7u;
}

static void log_map(Log *l, size_t size) {
    check(ftruncate(l->fd, (off_t)size) == 0, "ftruncate");
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, l->fd, 0);
    check(m != MAP_FAILED, "mmap");
    l->m = m;
    l->size = size;
}

static void log_open_new(Log *l, const char *path, size_t size) {
    l->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(l->fd >= 0, "open");
    log_map(l, size);
    wr32(l->m, LOG_MAGIC);
    wr32(l->m + 4, HDR);
    wr32(l->m + 8, 0);
}

static void log_close(Log *l) {
    check(msync(l->m, l->size, MS_SYNC) == 0, "msync");
    check(munmap(l->m, l->size) == 0, "munmap");
    close(l->fd);
}

/* Append: payload first, then publish by moving the committed offset. */
static int log_append_torn(Log *l, const void *data, uint32_t len, int commit, uint32_t partial) {
    uint32_t tail = rd32(l->m + 4);
    size_t span = rec_span(len);
    if (tail + span > l->size) {
        /* grow by doubling: remap and continue */
        check(munmap(l->m, l->size) == 0, "munmap for grow");
        log_map(l, l->size * 2);
    }
    unsigned char *r = l->m + tail;
    wr32(r, len);
    wr32(r + 4, crc32(data, len));
    memcpy(r + 8, data, partial < len ? partial : len); /* partial < len simulates a torn write */
    if (partial < len)
        memset(r + 8 + partial, 0, len - partial);
    if (!commit)
        return 0;
    wr32(l->m + 8, rd32(l->m + 8) + 1);
    wr32(l->m + 4, tail + (uint32_t)span);
    return 1;
}

static int log_append(Log *l, const void *data, uint32_t len) {
    return log_append_torn(l, data, len, 1, len);
}

/* Recovery: trust the committed offset, then also adopt fully valid records past it. */
static uint32_t log_recover(Log *l, uint32_t *adopted, uint32_t *rejected_tail) {
    uint32_t off = rd32(l->m + 4), count = rd32(l->m + 8);
    *adopted = 0;
    *rejected_tail = 0;
    for (;;) {
        if (off + 8 > l->size)
            break;
        uint32_t len = rd32(l->m + off), crc = rd32(l->m + off + 4);
        if (len == 0 || len > 4096 || off + rec_span(len) > l->size) {
            break;
        }
        if (crc32(l->m + off + 8, len) != crc) {
            *rejected_tail = 1;
            break;
        }
        off += (uint32_t)rec_span(len);
        count++;
        (*adopted)++;
    }
    wr32(l->m + 4, off);
    wr32(l->m + 8, count);
    return count;
}

static uint32_t walk(const Log *l, uint32_t *bytes_out) {
    uint32_t off = HDR, n = 0, payload = 0;
    uint32_t end = rd32(l->m + 4);
    while (off < end) {
        uint32_t len = rd32(l->m + off);
        check(crc32(l->m + off + 8, len) == rd32(l->m + off + 4), "record crc");
        payload += len;
        off += (uint32_t)rec_span(len);
        n++;
    }
    check(off == end, "walk ends exactly at commit offset");
    *bytes_out = payload;
    return n;
}

int main(void) {
    char path[] = "mmlog_XXXXXX";
    int tmpfd = mkstemp(path);
    check(tmpfd >= 0, "mkstemp");
    close(tmpfd);

    Log l;
    log_open_new(&l, path, 1024);
    size_t first_size = l.size;
    char msg[64];
    uint32_t total_payload = 0;
    for (int i = 0; i < 60; i++) {
        int n = snprintf(msg, sizeof msg, "event-%03d:%*s", i, (i * 7) % 20, "x");
        check(log_append(&l, msg, (uint32_t)n) == 1, "append");
        total_payload += (uint32_t)n;
    }
    printf("appended 60 records, payload=%u bytes, file grew: %s\n", (unsigned)total_payload,
           l.size > first_size ? "yes" : "no");
    uint32_t bytes = 0;
    uint32_t n = walk(&l, &bytes);
    check(n == 60 && bytes == total_payload, "walk totals");
    printf("committed count=%u tail-offset=%u\n", (unsigned)rd32(l.m + 8), (unsigned)rd32(l.m + 4));

    /* crash case 1: record fully written but the commit pointer never moved */
    log_append_torn(&l, "written-not-published", 21, 0, 21);
    /* crash case 2 (after recovery below): record cut off mid-payload */
    uint32_t adopted, torn;
    uint32_t count = log_recover(&l, &adopted, &torn);
    printf("recovery 1: adopted=%u torn=%u count=%u\n", (unsigned)adopted, (unsigned)torn, (unsigned)count);
    check(adopted == 1 && !torn && count == 61, "adopt complete record");

    log_append_torn(&l, "this-record-is-cut-short", 24, 0, 9);
    count = log_recover(&l, &adopted, &torn);
    printf("recovery 2: adopted=%u torn=%u count=%u\n", (unsigned)adopted, (unsigned)torn, (unsigned)count);
    check(adopted == 0 && torn == 1 && count == 61, "torn record dropped");

    /* the log remains appendable after recovery and overwrites the torn bytes */
    check(log_append(&l, "after-recovery", 14) == 1, "append after recovery");
    n = walk(&l, &bytes);
    printf("final records=%u payload=%u\n", (unsigned)n, (unsigned)bytes);
    check(n == 62, "final count");

    /* reopen from the file and verify the last record survived on disk */
    uint32_t tail = rd32(l.m + 4);
    size_t sz = l.size;
    log_close(&l);
    l.fd = open(path, O_RDWR);
    check(l.fd >= 0, "reopen");
    l.m = mmap(NULL, sz, PROT_READ, MAP_SHARED, l.fd, 0);
    check(l.m != MAP_FAILED, "mmap ro");
    l.size = sz;
    check(rd32(l.m) == LOG_MAGIC && rd32(l.m + 4) == tail, "header persisted");
    uint32_t last_off = HDR, prev = HDR;
    while (last_off < tail) {
        prev = last_off;
        last_off += (uint32_t)rec_span(rd32(l.m + last_off));
    }
    printf("last record: %.*s\n", (int)rd32(l.m + prev), (const char *)(l.m + prev + 8));
    check(memcmp(l.m + prev + 8, "after-recovery", 14) == 0, "last payload");
    munmap(l.m, sz);
    close(l.fd);
    unlink(path);
    return 0;
}
