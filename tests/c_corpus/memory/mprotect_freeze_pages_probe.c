/*
 * title: Read-only freezing of pages with protection state audit
 * topic: memory
 * covers: mprotect, PROT_READ toggling, write probes via pipe, freeze/thaw discipline
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NPAGES 8

static int pfd[2];

static int can_write(void *p) {
    unsigned char b = 1;
    if (write(pfd[1], &b, 1) != 1)
        return -1;
    if (read(pfd[0], p, 1) == 1)
        return 1;
    unsigned char sink;
    while (read(pfd[0], &sink, 1) == 1) {
    }
    return 0;
}

static int can_read(const void *p) {
    if (write(pfd[1], p, 1) == 1) {
        unsigned char sink;
        if (read(pfd[0], &sink, 1) != 1)
            return -1;
        return 1;
    }
    return 0;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef enum { P_RW, P_RO, P_NONE } Prot;

static const char *prot_name(Prot p) {
    return p == P_RW ? "rw" : p == P_RO ? "ro" : "--";
}

static int prot_flags(Prot p) {
    return p == P_RW ? (PROT_READ | PROT_WRITE) : p == P_RO ? PROT_READ : PROT_NONE;
}

int main(void) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    check(pipe(pfd) == 0, "pipe");
    check(fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK) == 0, "nonblock");

    unsigned char *base = mmap(NULL, pg * NPAGES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    check(base != MAP_FAILED, "mmap");

    /* tag each page with a value so we can confirm contents survive protection changes */
    for (int i = 0; i < NPAGES; i++)
        memset(base + (size_t)i * pg, 0x10 + i, pg);

    Prot want[NPAGES];
    for (int i = 0; i < NPAGES; i++)
        want[i] = P_RW;

    /* a scripted sequence of protection changes on page ranges */
    struct {
        int first, count;
        Prot to;
    } script[] = {
        {0, 4, P_RO}, {2, 2, P_NONE}, {3, 3, P_RW}, {6, 2, P_RO}, {0, 1, P_RW}, {5, 2, P_NONE}, {1, 1, P_RW},
    };
    for (size_t s = 0; s < sizeof script / sizeof script[0]; s++) {
        check(mprotect(base + (size_t)script[s].first * pg, (size_t)script[s].count * pg,
                       prot_flags(script[s].to)) == 0, "mprotect");
        for (int i = 0; i < script[s].count; i++)
            want[script[s].first + i] = script[s].to;
        printf("step %zu:", s);
        for (int i = 0; i < NPAGES; i++) {
            int r = can_read(base + (size_t)i * pg);
            int w = can_write(base + (size_t)i * pg + 1);
            char c = (r && w) ? 'W' : r ? 'R' : '-';
            char exp = want[i] == P_RW ? 'W' : want[i] == P_RO ? 'R' : '-';
            check(c == exp, "observed protection matches request");
            printf(" %c", c);
        }
        printf("\n");
    }

    /* restore everything and verify the page tags were never disturbed */
    check(mprotect(base, pg * NPAGES, PROT_READ | PROT_WRITE) == 0, "restore");
    for (int i = 0; i < NPAGES; i++) {
        unsigned char *p = base + (size_t)i * pg;
        unsigned char expect = (unsigned char)(0x10 + i);
        /* page 1 byte 1 may have been overwritten by can_write probes (value 1) */
        for (size_t k = 2; k < 64; k++)
            check(p[k] == expect, "tag intact");
        check(p[0] == expect, "byte 0 intact");
    }
    printf("final states:");
    for (int i = 0; i < NPAGES; i++)
        printf(" %s", prot_name(want[i]));
    printf("\n");

    /* a frozen page rejects a copy-in but accepts it after thaw */
    unsigned char *page = base + 3 * pg;
    check(mprotect(page, pg, PROT_READ) == 0, "freeze");
    printf("frozen write accepted: %d\n", can_write(page + 10));
    check(mprotect(page, pg, PROT_READ | PROT_WRITE) == 0, "thaw");
    printf("thawed write accepted: %d\n", can_write(page + 10));
    check(page[10] == 1, "probe byte landed");

    check(munmap(base, pg * NPAGES) == 0, "munmap");
    close(pfd[0]);
    close(pfd[1]);
    return 0;
}
