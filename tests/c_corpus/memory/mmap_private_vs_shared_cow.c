/*
 * title: MAP_PRIVATE copy-on-write versus MAP_SHARED write-through
 * topic: memory
 * covers: mmap sharing modes, copy-on-write isolation, file effects via pread, fork inheritance semantics
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define LEN 8192u

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t sum_file(int fd) {
    unsigned char buf[LEN];
    check(pread(fd, buf, LEN, 0) == (ssize_t)LEN, "pread");
    uint32_t s = 0;
    for (unsigned i = 0; i < LEN; i++)
        s = s * 131u + buf[i];
    return s;
}

static uint32_t sum_mem(const unsigned char *p) {
    uint32_t s = 0;
    for (unsigned i = 0; i < LEN; i++)
        s = s * 131u + p[i];
    return s;
}

int main(void) {
    char path[] = "mmcow_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    unsigned char init[LEN];
    for (unsigned i = 0; i < LEN; i++)
        init[i] = (unsigned char)(i * 7 + 3);
    check(write(fd, init, LEN) == (ssize_t)LEN, "write");
    uint32_t original = sum_file(fd);

    /* 1. private mapping: local writes never reach the file */
    unsigned char *priv = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(priv != MAP_FAILED, "private map");
    check(sum_mem(priv) == original, "private view starts equal to file");
    for (unsigned i = 0; i < LEN; i += 16)
        priv[i] = 0xEE;
    uint32_t priv_sum = sum_mem(priv);
    printf("private view changed: %s\n", priv_sum != original ? "yes" : "no");
    printf("file unchanged after private writes: %s\n", sum_file(fd) == original ? "yes" : "no");
    check(msync(priv, LEN, MS_SYNC) == 0, "msync private");
    check(sum_file(fd) == original, "msync of private mapping does not write back");

    /* 2. shared mapping: writes reach the file */
    unsigned char *shar = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(shar != MAP_FAILED, "shared map");
    for (unsigned i = 8; i < LEN; i += 64)
        shar[i] = 0x11;
    check(msync(shar, LEN, MS_SYNC) == 0, "msync shared");
    uint32_t after_shared = sum_file(fd);
    printf("file changed after shared writes: %s\n", after_shared != original ? "yes" : "no");
    check(after_shared == sum_mem(shar), "file equals shared view");

    /* 3. a page already privately modified is detached: later shared writes do not leak into it */
    check(sum_mem(priv) == priv_sum, "private pages stay independent");
    printf("private view isolated from shared writes: yes\n");

    /* 4. fork: MAP_PRIVATE|MAP_ANON is copied, MAP_SHARED|MAP_ANON is common */
    unsigned char *anon_priv = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned char *anon_shar = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    check(anon_priv != MAP_FAILED && anon_shar != MAP_FAILED, "anon maps");
    memset(anon_priv, 1, LEN);
    memset(anon_shar, 1, LEN);
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        memset(anon_priv, 2, LEN);
        memset(anon_shar, 2, LEN);
        /* the private file mapping is also a private copy in the child */
        priv[0] = 0x77;
        shar[1] = 0x66;
        _exit(0);
    }
    int st = 0;
    check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "child");
    printf("after child: private anon byte=%u shared anon byte=%u\n", (unsigned)anon_priv[100],
           (unsigned)anon_shar[100]);
    check(anon_priv[100] == 1 && anon_shar[100] == 2, "fork semantics");
    printf("private file byte0 in parent still 0xEE: %s\n", priv[0] == 0xEE ? "yes" : "no");
    check(priv[0] == 0xEE, "child private write invisible");
    printf("shared file byte1 seen by parent: 0x%02x\n", (unsigned)shar[1]);
    check(shar[1] == 0x66, "child shared write visible");
    unsigned char b;
    check(pread(fd, &b, 1, 1) == 1 && b == 0x66, "file has child's write");

    munmap(priv, LEN);
    munmap(shar, LEN);
    munmap(anon_priv, LEN);
    munmap(anon_shar, LEN);
    close(fd);
    unlink(path);
    return 0;
}
