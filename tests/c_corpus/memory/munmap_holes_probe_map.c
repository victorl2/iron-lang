/*
 * title: Punching holes in a mapping and auditing it with kernel probes
 * topic: memory
 * covers: partial munmap, non-faulting mapped/unmapped probes via pipe write EFAULT, run-length map of an address range
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

#define NPAGES 16

static int pfd[2];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/*
 * The kernel copies one byte out of the address into a pipe. An unmapped
 * address fails with EFAULT instead of delivering a signal to us.
 */
static int page_mapped(const void *p) {
    errno = 0;
    ssize_t n = write(pfd[1], p, 1);
    if (n == 1) {
        unsigned char sink;
        check(read(pfd[0], &sink, 1) == 1, "drain");
        return 1;
    }
    check(n == -1 && errno == EFAULT, "unmapped page reports EFAULT");
    return 0;
}

static void run_lengths(unsigned char *base, size_t pg, char *out, size_t cap) {
    size_t pos = 0;
    int i = 0;
    while (i < NPAGES) {
        int m = page_mapped(base + (size_t)i * pg);
        int j = i;
        while (j < NPAGES && page_mapped(base + (size_t)j * pg) == m)
            j++;
        int w = snprintf(out + pos, cap - pos, "%s%d ", m ? "M" : "H", j - i);
        pos += (size_t)w;
        i = j;
    }
    if (pos)
        out[pos - 1] = 0;
}

int main(void) {
    check(pipe(pfd) == 0, "pipe");
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *base = mmap(NULL, pg * NPAGES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(base != MAP_FAILED, "mmap");
    for (int i = 0; i < NPAGES; i++)
        memset(base + (size_t)i * pg, 0x40 + i, pg);

    char runs[128];
    run_lengths(base, pg, runs, sizeof runs);
    printf("initial:   %s\n", runs);
    check(strcmp(runs, "M16") == 0, "fully mapped");

    /* a script of hole punches; the map must track it exactly */
    struct {
        int first, count;
    } punch[] = {{4, 2}, {10, 1}, {0, 1}, {6, 3}, {14, 2}};
    unsigned char want[NPAGES];
    memset(want, 1, sizeof want);
    for (size_t s = 0; s < sizeof punch / sizeof punch[0]; s++) {
        check(munmap(base + (size_t)punch[s].first * pg, (size_t)punch[s].count * pg) == 0, "punch");
        for (int k = 0; k < punch[s].count; k++)
            want[punch[s].first + k] = 0;
        run_lengths(base, pg, runs, sizeof runs);
        printf("punch %zu:   %s\n", s, runs);
        for (int i = 0; i < NPAGES; i++)
            check(page_mapped(base + (size_t)i * pg) == want[i], "map matches script");
    }

    /* survivors keep their contents; both edges of each surviving page are probed */
    int alive = 0;
    for (int i = 0; i < NPAGES; i++) {
        if (!want[i])
            continue;
        check(base[(size_t)i * pg] == 0x40 + i && base[(size_t)i * pg + pg - 1] == 0x40 + i, "survivor intact");
        check(page_mapped(base + (size_t)i * pg) && page_mapped(base + (size_t)i * pg + pg - 1), "edges mapped");
        alive++;
    }
    printf("surviving pages: %d\n", alive);

    /* holes are unmapped at both edges too */
    int hole_edges = 0;
    for (int i = 0; i < NPAGES; i++)
        if (!want[i]) {
            check(!page_mapped(base + (size_t)i * pg) && !page_mapped(base + (size_t)i * pg + pg - 1), "hole edges");
            hole_edges += 2;
        }
    printf("hole edge probes: %d\n", hole_edges);

    /* unmapping an already-unmapped range is harmless */
    check(munmap(base + 4 * pg, 2 * pg) == 0, "double munmap ok");
    printf("munmap of hole: ok\n");

    /* a hole can be re-filled with a fixed mapping */
    void *fill = mmap(base + 4 * pg, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    check(fill == base + 4 * pg, "refill hole");
    want[4] = 1;
    run_lengths(base, pg, runs, sizeof runs);
    printf("refilled:  %s\n", runs);
    check(base[4 * pg] == 0, "refilled page is zero");

    /* remove the rest, page by page, from the back */
    for (int i = NPAGES - 1; i >= 0; i--)
        if (want[i]) {
            check(munmap(base + (size_t)i * pg, pg) == 0, "munmap survivor");
            want[i] = 0;
        }
    int any = 0;
    for (int i = 0; i < NPAGES; i++)
        any += page_mapped(base + (size_t)i * pg);
    printf("mapped pages remaining: %d\n", any);
    check(any == 0, "all gone");
    close(pfd[0]);
    close(pfd[1]);
    return 0;
}
