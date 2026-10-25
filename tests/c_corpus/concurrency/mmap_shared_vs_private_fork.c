/*
 * title: Shared and private mappings across fork
 * topic: concurrency
 * covers: mmap MAP_SHARED vs MAP_PRIVATE, copy-on-write, disjoint slices per process, no-lock parallel fill
 * deps: libc, posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { NPROC = 5, SLICE = 4096 };

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned mix(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

int main(void) {
    size_t n = (size_t)NPROC * SLICE;
    unsigned *shared = mmap(NULL, n * sizeof *shared, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    unsigned *priv = mmap(NULL, n * sizeof *priv, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned long *partial = mmap(NULL, NPROC * sizeof *partial, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(shared != MAP_FAILED && priv != MAP_FAILED && partial != MAP_FAILED, "mmap");
    for (size_t i = 0; i < n; i++) {
        shared[i] = 0;
        priv[i] = 0xAAAAAAAAu; /* parent's value, must survive the children's writes */
    }
    memset(partial, 0, NPROC * sizeof *partial);

    fflush(stdout);
    pid_t pids[NPROC];
    for (int p = 0; p < NPROC; p++) {
        pids[p] = fork();
        check(pids[p] >= 0, "fork");
        if (pids[p] == 0) {
            unsigned long sum = 0;
            for (size_t i = (size_t)p * SLICE; i < (size_t)(p + 1) * SLICE; i++) {
                unsigned v = mix((unsigned)i + 1000u * (unsigned)p);
                shared[i] = v;      /* visible to the parent */
                priv[i] = v;        /* copy-on-write: stays in this child */
                sum += v;
            }
            partial[p] = sum;
            /* the child sees its own private writes */
            for (size_t i = (size_t)p * SLICE; i < (size_t)(p + 1) * SLICE; i++)
                if (priv[i] != shared[i])
                    _exit(1);
            _exit(0);
        }
    }
    for (int p = 0; p < NPROC; p++) {
        int st = 0;
        check(waitpid(pids[p], &st, 0) == pids[p] && WIFEXITED(st) && WEXITSTATUS(st) == 0, "child ok");
    }
    unsigned long grand = 0;
    size_t private_untouched = 0;
    for (int p = 0; p < NPROC; p++) {
        unsigned long expect = 0;
        for (size_t i = (size_t)p * SLICE; i < (size_t)(p + 1) * SLICE; i++) {
            unsigned v = mix((unsigned)i + 1000u * (unsigned)p);
            expect += v;
            check(shared[i] == v, "shared slice written by child");
            if (priv[i] == 0xAAAAAAAAu)
                private_untouched++;
        }
        check(partial[p] == expect, "partial sum");
        printf("slice %d: partial sum %lu\n", p, partial[p]);
        grand += partial[p];
    }
    printf("total of all slices = %lu\n", grand);
    printf("private mapping still holds parent's value in %zu of %zu cells\n", private_untouched, n);
    check(private_untouched == n, "MAP_PRIVATE writes stay private");
    munmap(shared, n * sizeof *shared);
    munmap(priv, n * sizeof *priv);
    munmap(partial, NPROC * sizeof *partial);
    return 0;
}
