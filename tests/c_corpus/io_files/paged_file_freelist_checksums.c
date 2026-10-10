/*
 * title: Fixed-size page file with header, free list and per-page checksums
 * topic: io_files
 * covers: pager, page allocation, free list chain, page checksum, header page, reuse after free, pread/pwrite style access via fseek, integrity scan
 * deps: libc, posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

static uint32_t crc_tab[256];
void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
}
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n) {
    const unsigned char *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
uint32_t crc32_of(const void *buf, size_t n) { return crc32_update(0, buf, n); }

#include <unistd.h>
#include <fcntl.h>

enum { PAGE = 128, NPAGES_MAX = 64, DATA = PAGE - 12 };
/* page layout: u32 crc | u32 type/next | u32 used | data[DATA]; page 0 is the header page: magic, page count, free head, root */
enum { T_FREE = 1, T_DATA = 2, T_HEADER = 3 };

static int fd = -1;

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void page_write(uint32_t no, uint32_t type, uint32_t next_or_used, const unsigned char *data, size_t len) {
    unsigned char pg[PAGE];
    memset(pg, 0, sizeof pg);
    put32(pg + 4, type);
    put32(pg + 8, next_or_used);
    if (len) memcpy(pg + 12, data, len);
    put32(pg, crc32_of(pg + 4, PAGE - 4));
    CHECK(pwrite(fd, pg, PAGE, (off_t)no * PAGE) == PAGE);
}

/* returns 0 ok, 1 crc bad, 2 short read */
static int page_read(uint32_t no, unsigned char *pg) {
    ssize_t r = pread(fd, pg, PAGE, (off_t)no * PAGE);
    if (r != PAGE) return 2;
    return get32(pg) == crc32_of(pg + 4, PAGE - 4) ? 0 : 1;
}

typedef struct { uint32_t npages, free_head, root, live; } Hdr;

static void hdr_write(const Hdr *h) {
    unsigned char d[16];
    put32(d, h->npages); put32(d + 4, h->free_head); put32(d + 8, h->root); put32(d + 12, 0x49524F4Eu);
    page_write(0, T_HEADER, 0, d, 16);
}
static void hdr_read(Hdr *h) {
    unsigned char pg[PAGE];
    CHECK(page_read(0, pg) == 0);
    h->npages = get32(pg + 12); h->free_head = get32(pg + 16); h->root = get32(pg + 20);
    CHECK(get32(pg + 24) == 0x49524F4Eu);
}

static uint32_t alloc_page(Hdr *h) {
    if (h->free_head) {
        uint32_t no = h->free_head;
        unsigned char pg[PAGE];
        CHECK(page_read(no, pg) == 0 && get32(pg + 4) == T_FREE);
        h->free_head = get32(pg + 8);
        return no;
    }
    CHECK(h->npages < NPAGES_MAX);
    return h->npages++;
}

static void free_page(Hdr *h, uint32_t no) {
    page_write(no, T_FREE, h->free_head, NULL, 0);
    h->free_head = no;
}

/* A blob is a chain of data pages; header field 'next' lives in the third word via a 4-byte prefix in data. */
static uint32_t blob_store(Hdr *h, const unsigned char *d, size_t n) {
    uint32_t first = 0, prev = 0;
    size_t off = 0;
    unsigned char buf[DATA];
    do {
        uint32_t no = alloc_page(h);
        size_t l = n - off < DATA - 4 ? n - off : DATA - 4;
        put32(buf, 0);                       /* next pointer, patched below */
        memcpy(buf + 4, d + off, l);
        page_write(no, T_DATA, (uint32_t)l, buf, l + 4);
        if (prev) {
            unsigned char pg[PAGE];
            CHECK(page_read(prev, pg) == 0);
            memcpy(buf, pg + 12, DATA);
            put32(buf, no);
            page_write(prev, T_DATA, get32(pg + 8), buf, DATA);
        } else first = no;
        prev = no;
        off += l;
    } while (off < n);
    return first;
}

static size_t blob_load(uint32_t first, unsigned char *out, size_t cap, int *pages, int *err) {
    size_t n = 0;
    *pages = 0;
    for (uint32_t no = first; no;) {
        unsigned char pg[PAGE];
        int r = page_read(no, pg);
        if (r) { *err = r; return n; }
        uint32_t used = get32(pg + 8);
        if (get32(pg + 4) != T_DATA || used > DATA - 4 || n + used > cap) { *err = 3; return n; }
        memcpy(out + n, pg + 16, used);
        n += used;
        (*pages)++;
        no = get32(pg + 12);
    }
    return n;
}

static void blob_free(Hdr *h, uint32_t first) {
    for (uint32_t no = first; no;) {
        unsigned char pg[PAGE];
        CHECK(page_read(no, pg) == 0);
        uint32_t next = get32(pg + 12);
        free_page(h, no);
        no = next;
    }
}

static int chain_len(const Hdr *h) {
    int c = 0;
    for (uint32_t no = h->free_head; no; c++) {
        unsigned char pg[PAGE];
        CHECK(page_read(no, pg) == 0 && get32(pg + 4) == T_FREE && c < NPAGES_MAX);
        no = get32(pg + 8);
    }
    return c;
}

static void print_state(const char *what, const Hdr *h) {
    printf("%-26s pages=%2u free_chain=%d file=%ld bytes\n", what, (unsigned)h->npages, chain_len(h), (long)lseek(fd, 0, SEEK_END));
}

int main(void) {
    crc_init();
    fd = open("db.pages", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    Hdr h = {1, 0, 0, 0};
    hdr_write(&h);
    unsigned char blobs[5][700], back[800];
    static const size_t lens[5] = {50, 300, 700, 1, 200};
    uint32_t first[5];
    for (int i = 0; i < 5; i++) {
        for (size_t k = 0; k < lens[i]; k++) blobs[i][k] = (unsigned char)(rnd() >> 10);
        first[i] = blob_store(&h, blobs[i], lens[i]);
    }
    hdr_write(&h);
    print_state("after storing 5 blobs", &h);
    for (int i = 0; i < 5; i++) {
        int pages, err = 0;
        size_t n = blob_load(first[i], back, sizeof back, &pages, &err);
        CHECK(!err && n == lens[i] && !memcmp(back, blobs[i], n));
        printf("blob %d: %3zu bytes in %d pages, first page %u\n", i, n, pages, (unsigned)first[i]);
    }
    blob_free(&h, first[1]);
    blob_free(&h, first[3]);
    hdr_write(&h);
    print_state("after freeing blobs 1,3", &h);
    /* reuse: a new blob should fill freed pages before growing the file */
    uint32_t before = h.npages;
    unsigned char big[500];
    for (size_t k = 0; k < sizeof big; k++) big[k] = (unsigned char)(rnd() >> 4);
    uint32_t nf = blob_store(&h, big, sizeof big);
    hdr_write(&h);
    print_state("after storing 500 bytes", &h);
    printf("file grew by %u pages\n", (unsigned)(h.npages - before));
    int pages, err = 0;
    size_t n = blob_load(nf, back, sizeof back, &pages, &err);
    CHECK(!err && n == sizeof big && !memcmp(back, big, n));
    /* reopen from disk and verify header */
    close(fd);
    fd = open("db.pages", O_RDWR);
    CHECK(fd >= 0);
    Hdr h2;
    hdr_read(&h2);
    CHECK(h2.npages == h.npages && h2.free_head == h.free_head);
    /* integrity scan, then corrupt one byte in a live page and one in a free page */
    int bad = 0;
    for (uint32_t p = 0; p < h2.npages; p++) { unsigned char pg[PAGE]; if (page_read(p, pg)) bad++; }
    printf("integrity scan: %u pages, %d bad\n", (unsigned)h2.npages, bad);
    CHECK(bad == 0);
    unsigned char b1 = 0;
    CHECK(pread(fd, &b1, 1, (off_t)first[0] * PAGE + 40) == 1);
    b1 ^= 0x20;
    CHECK(pwrite(fd, &b1, 1, (off_t)first[0] * PAGE + 40) == 1);
    err = 0;
    blob_load(first[0], back, sizeof back, &pages, &err);
    printf("corrupted live page %u: read error code %d\n", (unsigned)first[0], err);
    CHECK(err == 1);
    for (uint32_t p = 1; p < h2.npages; p++) { unsigned char pg[PAGE]; if (page_read(p, pg)) printf("scan flags page %u\n", (unsigned)p); }
    /* truncate the file in the middle of a page: short read is distinguished from crc failure */
    CHECK(ftruncate(fd, (off_t)h2.npages * PAGE - 50) == 0);
    unsigned char pg[PAGE];
    printf("last page after truncation: read code %d, previous page code %d\n", page_read(h2.npages - 1, pg), page_read(h2.npages - 2, pg));
    close(fd);
    remove("db.pages");
    return 0;
}
