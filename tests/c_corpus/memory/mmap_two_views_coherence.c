/*
 * title: Two overlapping shared windows on one file stay coherent
 * topic: memory
 * covers: multiple MAP_SHARED views, page-aligned file offsets, coherence without msync, window arithmetic
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    unsigned char *p;
    size_t off, len;
} Window;

static Window window_open(int fd, size_t off, size_t len) {
    Window w = {NULL, off, len};
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    check(m != MAP_FAILED, "mmap window");
    w.p = m;
    return w;
}

/* translate a file offset into the window, or NULL when it is outside */
static unsigned char *at(const Window *w, size_t file_off) {
    if (file_off < w->off || file_off >= w->off + w->len)
        return NULL;
    return w->p + (file_off - w->off);
}

int main(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    char path[] = "mmvw_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    size_t total = pg * 8;
    check(ftruncate(fd, (off_t)total) == 0, "ftruncate");

    /* window A: pages 0..4, window B: pages 3..7 : overlap is pages 3 and 4 */
    Window A = window_open(fd, 0, pg * 5);
    Window B = window_open(fd, pg * 3, pg * 5);
    check(A.p != B.p, "distinct addresses");

    /* every file offset that both windows cover maps to the same byte */
    size_t overlap_lo = B.off, overlap_hi = A.off + A.len;
    printf("overlap pages: %zu\n", (overlap_hi - overlap_lo) / pg);

    unsigned long agree = 0;
    for (size_t off = overlap_lo; off < overlap_hi; off += 251) {
        *at(&A, off) = (unsigned char)(off * 13 + 5);
        check(*at(&B, off) == (unsigned char)(off * 13 + 5), "A write seen in B");
        *at(&B, off + 1) = (unsigned char)(off * 7 + 1);
        check(*at(&A, off + 1) == (unsigned char)(off * 7 + 1), "B write seen in A");
        agree += 2;
    }
    printf("cross-window writes observed immediately: %lu\n", agree);

    /* fill B only, and read it all back through a positional file read: no msync needed for coherence */
    for (size_t i = 0; i < B.len; i++)
        B.p[i] = (unsigned char)((B.off + i) % 251);
    unsigned char *buf = malloc(total);
    check(buf != NULL, "malloc");
    check(pread(fd, buf, total, 0) == (ssize_t)total, "pread all");
    unsigned long mismatch = 0;
    for (size_t i = B.off; i < B.off + B.len; i++)
        if (buf[i] != (unsigned char)(i % 251))
            mismatch++;
    printf("pread vs window B mismatches: %lu\n", mismatch);
    check(mismatch == 0, "read(2) sees mapped writes");

    /* and the other direction: pwrite shows up in both windows where they cover the offset */
    unsigned char patch[3] = {0xC1, 0xC2, 0xC3};
    size_t o = pg * 4 - 1; /* straddles the middle of the overlap */
    check(pwrite(fd, patch, 3, (off_t)o) == 3, "pwrite");
    for (int k = 0; k < 3; k++) {
        check(*at(&A, o + (size_t)k) == patch[k] && *at(&B, o + (size_t)k) == patch[k], "pwrite in both views");
    }
    printf("pwrite visible in both windows: yes\n");

    /* bytes only covered by one window are invisible to the other */
    check(at(&B, 0) == NULL && at(&A, total - 1) == NULL, "outside windows");
    A.p[10] = 0x5A;
    unsigned char b10;
    check(pread(fd, &b10, 1, 10) == 1 && b10 == 0x5A, "A-only byte on disk");
    printf("windows cover file offsets [0,%zu) and [%zu,%zu) in pages\n", A.len / pg, B.off / pg,
           (B.off + B.len) / pg);

    free(buf);
    check(munmap(A.p, A.len) == 0 && munmap(B.p, B.len) == 0, "munmap");
    close(fd);
    unlink(path);
    return 0;
}
