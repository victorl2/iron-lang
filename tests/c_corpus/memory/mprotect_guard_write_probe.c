/*
 * title: Guard pages verified by non-faulting kernel probes
 * topic: memory
 * covers: mprotect, PROT_NONE guard pages, EFAULT probing via pipe read/write, overflow-safe buffers
 * deps: posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * A page is probed without touching it from user code: the kernel copies a
 * byte to or from it through a pipe, and reports EFAULT instead of raising
 * SIGSEGV when the protection forbids the access.
 */
static int pfd[2];

static int can_read(const void *p) {
    ssize_t n = write(pfd[1], p, 1);
    if (n == 1) {
        unsigned char sink;
        if (read(pfd[0], &sink, 1) != 1)
            return -1;
        return 1;
    }
    return 0;
}

static int can_write(void *p) {
    unsigned char b = 0x5A;
    if (write(pfd[1], &b, 1) != 1)
        return -1;
    ssize_t n = read(pfd[0], p, 1);
    if (n == 1)
        return 1;
    /* drain the byte that was not delivered */
    unsigned char sink;
    while (read(pfd[0], &sink, 1) == 1) {
    }
    return 0;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* A buffer of `cap` bytes whose end abuts a PROT_NONE guard page. */
typedef struct {
    unsigned char *map;
    size_t maplen;
    unsigned char *data;
    size_t cap;
} GuardBuf;

static int guard_new(GuardBuf *g, size_t cap, size_t pg) {
    size_t body = (cap + pg - 1) / pg * pg;
    g->maplen = body + 2 * pg;
    g->map = mmap(NULL, g->maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g->map == MAP_FAILED)
        return -1;
    if (mprotect(g->map, pg, PROT_NONE) != 0)
        return -1;
    if (mprotect(g->map + pg + body, pg, PROT_NONE) != 0)
        return -1;
    g->data = g->map + pg + body - cap; /* right-aligned against the tail guard */
    g->cap = cap;
    return 0;
}

/* checked write that consults the probe first, so an overflow is caught not crashed */
static int guarded_store(GuardBuf *g, size_t off, unsigned char v) {
    unsigned char *p = g->data + off;
    if (!can_write(p))
        return -1;
    *p = v;
    return 0;
}

int main(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    check(pipe(pfd) == 0, "pipe");
    int fl = fcntl(pfd[0], F_GETFL);
    check(fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK) == 0, "nonblock");

    GuardBuf g = {NULL, 0, NULL, 0};
    check(guard_new(&g, 100, pg) == 0, "guard_new");

    int head_ok = can_read(g.map);
    int tail_ok = can_read(g.data + g.cap);
    printf("head guard readable: %d\n", head_ok);
    printf("tail guard readable: %d\n", tail_ok);
    check(head_ok == 0 && tail_ok == 0, "guards unreadable");
    printf("first byte writable: %d\n", can_write(g.data));
    printf("last byte writable: %d\n", can_write(g.data + g.cap - 1));
    printf("one past end writable: %d\n", can_write(g.data + g.cap));
    printf("start of body page readable: %d\n", can_read(g.map + pg));

    /* fill the buffer, then walk past the end: the probe stops the walk */
    size_t stored = 0;
    for (size_t i = 0; i < g.cap + 50; i++) {
        if (guarded_store(&g, i, (unsigned char)(i * 3 + 1)) != 0)
            break;
        stored++;
    }
    printf("stores accepted before guard: %zu\n", stored);
    check(stored == g.cap, "stopped exactly at capacity");
    unsigned sum = 0;
    for (size_t i = 0; i < g.cap; i++)
        sum += g.data[i];
    printf("buffer sum: %u\n", sum);

    /* mprotect toggles: open the tail guard, use it, close it again */
    unsigned char *tail = g.data + g.cap;
    unsigned char *tail_page = g.map + g.maplen - pg;
    check(tail >= g.map && tail <= tail_page, "tail placement");
    check(mprotect(tail_page, pg, PROT_READ | PROT_WRITE) == 0, "open guard");
    printf("opened tail guard writable: %d\n", can_write(tail_page));
    check(mprotect(tail_page, pg, PROT_NONE) == 0, "close guard");
    printf("closed tail guard writable: %d\n", can_write(tail_page));

    /* bad mprotect arguments are rejected with EINVAL for a misaligned address */
    errno = 0;
    int r = mprotect(g.map + 1, pg, PROT_READ);
    printf("misaligned mprotect: %s\n", (r == -1 && errno == EINVAL) ? "EINVAL" : "other");
    check(r == -1 && errno == EINVAL, "misaligned rejected");

    check(munmap(g.map, g.maplen) == 0, "munmap");
    close(pfd[0]);
    close(pfd[1]);
    return 0;
}
