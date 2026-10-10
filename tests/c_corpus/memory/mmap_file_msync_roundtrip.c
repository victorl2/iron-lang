/*
 * title: File-backed mmap write, msync and read-back
 * topic: memory
 * covers: mmap MAP_SHARED, ftruncate, msync, pread verification, remap
 * deps: posix
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NREC 512u

typedef struct {
    uint32_t id;
    uint32_t sum;
    unsigned char payload[24];
} Rec; /* 32 bytes, no padding */

static uint64_t rng_state = 42;

static uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t sum_bytes(const unsigned char *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++)
        s = s * 31u + p[i];
    return s;
}

int main(void) {
    char path[] = "mmrt_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t bytes = NREC * sizeof(Rec);
    check(sizeof(Rec) == 32, "record size");
    check(ftruncate(fd, (off_t)bytes) == 0, "ftruncate");

    Rec *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "mmap");
    /* file starts out as zeros */
    check(map[0].id == 0 && map[NREC - 1].sum == 0, "sparse zero");

    for (uint32_t i = 0; i < NREC; i++) {
        map[i].id = i * 7u + 1u;
        for (int k = 0; k < 24; k++)
            map[i].payload[k] = (unsigned char)(rnd() >> 24);
        map[i].sum = sum_bytes(map[i].payload, 24);
    }
    check(msync(map, bytes, MS_SYNC) == 0, "msync");

    /* read back through the descriptor, bypassing the mapping */
    unsigned long good = 0;
    unsigned long total = 0;
    for (uint32_t i = 0; i < NREC; i++) {
        Rec r;
        ssize_t n = pread(fd, &r, sizeof r, (off_t)(i * sizeof(Rec)));
        check(n == (ssize_t)sizeof r, "pread");
        check(r.id == i * 7u + 1u, "id via pread");
        if (sum_bytes(r.payload, 24) == r.sum)
            good++;
        total += r.sum & 0xffu;
    }
    printf("records verified via pread: %lu of %u\n", good, NREC);
    check(good == NREC, "all sums");
    printf("low-byte checksum total: %lu\n", total);

    /* modify through pwrite and observe through the shared mapping */
    Rec patch;
    memset(&patch, 0, sizeof patch);
    patch.id = 0xDEADu;
    patch.sum = 0xBEEFu;
    check(pwrite(fd, &patch, sizeof patch, (off_t)(100 * sizeof(Rec))) == (ssize_t)sizeof patch, "pwrite");
    check(map[100].id == 0xDEADu && map[100].sum == 0xBEEFu, "pwrite visible through map");
    printf("pwrite visible in mapping: id=%u sum=%u\n", (unsigned)map[100].id, (unsigned)map[100].sum);

    check(munmap(map, bytes) == 0, "munmap");

    /* remap read-only and confirm persistence */
    const Rec *ro = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    check(ro != MAP_FAILED, "mmap ro");
    uint32_t idsum = 0;
    for (uint32_t i = 0; i < NREC; i++)
        idsum += ro[i].id;
    printf("id sum after remap: %u\n", (unsigned)idsum);
    uint32_t expect = 0;
    for (uint32_t i = 0; i < NREC; i++)
        expect += (i == 100) ? 0xDEADu : i * 7u + 1u;
    check(idsum == expect, "id sum");
    check(munmap((void *)ro, bytes) == 0, "munmap ro");

    /* growing the file zero-extends it */
    check(ftruncate(fd, (off_t)(bytes + 64)) == 0, "grow");
    unsigned char tail[64];
    memset(tail, 0xAA, sizeof tail);
    check(pread(fd, tail, sizeof tail, (off_t)bytes) == (ssize_t)sizeof tail, "tail read");
    int zero = 1;
    for (size_t i = 0; i < sizeof tail; i++)
        if (tail[i] != 0)
            zero = 0;
    printf("grown tail is zero: %s\n", zero ? "yes" : "no");
    check(zero, "tail zero");

    close(fd);
    unlink(path);
    return 0;
}
