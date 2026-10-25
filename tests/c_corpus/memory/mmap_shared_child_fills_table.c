/*
 * title: MAP_SHARED anonymous table filled by forked children
 * topic: memory
 * covers: MAP_SHARED|MAP_ANON, fork, waitpid, disjoint partitions, parent verification
 * deps: posix
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define NCHILD 4
#define PER 1024

typedef struct {
    uint32_t magic;
    uint32_t child;
    uint64_t checksum;
    uint32_t values[PER];
} Part;

static uint32_t value_for(uint32_t child, uint32_t i) {
    uint32_t x = child * 0x9E3779B1u + i * 0x85EBCA6Bu + 12345u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

int main(void) {
    size_t bytes = sizeof(Part) * NCHILD;
    Part *parts = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(parts != MAP_FAILED, "mmap shared");
    fflush(stdout);
    pid_t pids[NCHILD];
    for (int c = 0; c < NCHILD; c++) {
        pid_t pid = fork();
        check(pid >= 0, "fork");
        if (pid == 0) {
            Part *p = &parts[c];
            uint64_t sum = 0;
            for (uint32_t i = 0; i < PER; i++) {
                p->values[i] = value_for((uint32_t)c, i);
                sum += p->values[i];
            }
            p->child = (uint32_t)c;
            p->checksum = sum;
            p->magic = 0xC0FFEEu; /* written last: acts as a ready flag */
            _exit(10 + c);
        }
        pids[c] = pid;
    }
    for (int c = 0; c < NCHILD; c++) {
        int st = 0;
        check(waitpid(pids[c], &st, 0) == pids[c], "waitpid");
        check(WIFEXITED(st), "exited");
        printf("child %d exit code %d\n", c, WEXITSTATUS(st));
        check(WEXITSTATUS(st) == 10 + c, "exit code");
    }
    uint64_t grand = 0;
    for (int c = 0; c < NCHILD; c++) {
        const Part *p = &parts[c];
        check(p->magic == 0xC0FFEEu, "magic");
        check(p->child == (uint32_t)c, "child id");
        uint64_t sum = 0;
        for (uint32_t i = 0; i < PER; i++) {
            check(p->values[i] == value_for((uint32_t)c, i), "value");
            sum += p->values[i];
        }
        check(sum == p->checksum, "checksum");
        printf("part %d: checksum=%llu first=%u last=%u\n", c, (unsigned long long)sum,
               (unsigned)p->values[0], (unsigned)p->values[PER - 1]);
        grand += sum;
    }
    printf("grand total: %llu\n", (unsigned long long)grand);
    check(munmap(parts, bytes) == 0, "munmap");
    return 0;
}
