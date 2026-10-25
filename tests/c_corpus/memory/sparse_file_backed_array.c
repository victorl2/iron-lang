/*
 * title: Sparse file-backed array with lazily touched pages
 * topic: memory
 * covers: ftruncate sparse file, mmap of a huge index space, occupancy bitmap, page-granular commit accounting
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ELEMS ((size_t)1 << 20) /* one million slots */
#define CHUNK 512u              /* elements per logical chunk (4096 bytes at 8 bytes each) */

typedef uint64_t Elem;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint64_t rs = 0x1234567;

static uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void) {
    char path[] = "mmsp_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t bytes = ELEMS * sizeof(Elem);
    check(ftruncate(fd, (off_t)bytes) == 0, "ftruncate");
    Elem *a = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(a != MAP_FAILED, "mmap");

    /* logical occupancy is tracked separately, so we never read untouched chunks to find out */
    size_t nchunks = ELEMS / CHUNK;
    unsigned char *touched = calloc(nchunks, 1);
    check(touched != NULL, "calloc");

    /* write sparse elements, clustered so that only some chunks are ever written */
    enum { WRITES = 3000 };
    static size_t idx[WRITES];
    unsigned long chunk_hits = 0;
    for (int i = 0; i < WRITES; i++) {
        uint64_t r = rnd();
        size_t cluster = (size_t)(r % 64) * (nchunks / 64); /* 64 hot regions */
        size_t chunk = cluster + (size_t)((r >> 20) % 4);
        size_t slot = (size_t)((r >> 40) % CHUNK);
        idx[i] = chunk * CHUNK + slot;
        a[idx[i]] = (Elem)i * 2654435761u + 1;
        if (!touched[chunk]) {
            touched[chunk] = 1;
            chunk_hits++;
        }
    }
    /* later writes may overwrite earlier ones at the same index; compute the surviving values */
    static Elem want[WRITES];
    for (int i = 0; i < WRITES; i++) {
        want[i] = (Elem)i * 2654435761u + 1;
        for (int j = i + 1; j < WRITES; j++)
            if (idx[j] == idx[i]) {
                want[i] = 0;
                break;
            }
    }
    unsigned long verified = 0, overwritten = 0;
    for (int i = 0; i < WRITES; i++) {
        if (want[i] == 0) {
            overwritten++;
            continue;
        }
        check(a[idx[i]] == want[i], "value");
        verified++;
    }
    printf("elements=%zu chunks=%zu\n", (size_t)ELEMS, nchunks);
    printf("writes=%d distinct-verified=%lu overwritten=%lu\n", WRITES, verified, overwritten);
    printf("chunks touched: %lu of %zu\n", chunk_hits, nchunks);

    /* an untouched region reads as zero without having been written */
    size_t zeros = 0;
    for (size_t c = 0; c < nchunks; c++) {
        if (touched[c])
            continue;
        if (c % 37 == 0) {
            for (size_t k = 0; k < CHUNK; k++)
                zeros += (a[c * CHUNK + k] == 0);
        }
    }
    printf("sampled untouched chunk elements all zero: %zu\n", zeros);

    /* count non-zero elements by scanning the whole file through pread, chunk by chunk */
    check(msync(a, bytes, MS_SYNC) == 0, "msync");
    Elem buf[CHUNK];
    unsigned long nonzero = 0;
    unsigned long chunks_with_data = 0;
    for (size_t c = 0; c < nchunks; c++) {
        check(pread(fd, buf, sizeof buf, (off_t)(c * CHUNK * sizeof(Elem))) == (ssize_t)sizeof buf, "pread");
        unsigned long here = 0;
        for (size_t k = 0; k < CHUNK; k++)
            here += buf[k] != 0;
        nonzero += here;
        if (here)
            chunks_with_data++;
        check((here != 0) == (touched[c] != 0) || here == 0, "data only in touched chunks");
    }
    printf("non-zero elements via pread: %lu in %lu chunks\n", nonzero, chunks_with_data);
    check(nonzero == verified, "non-zero equals distinct writes");

    /* first and last element addressable, file length unchanged */
    a[0] = 111;
    a[ELEMS - 1] = 222;
    Elem edge[2];
    check(pread(fd, &edge[0], sizeof(Elem), 0) == (ssize_t)sizeof(Elem), "pread first");
    check(pread(fd, &edge[1], sizeof(Elem), (off_t)(bytes - sizeof(Elem))) == (ssize_t)sizeof(Elem), "pread last");
    check(edge[0] == 111 && edge[1] == 222, "edges");
    printf("edge elements: %llu %llu\n", (unsigned long long)edge[0], (unsigned long long)edge[1]);

    check(munmap(a, bytes) == 0, "munmap");
    free(touched);
    close(fd);
    unlink(path);
    return 0;
}
