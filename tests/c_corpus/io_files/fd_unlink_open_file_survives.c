/*
 * title: Unlinked and replaced files stay usable through open descriptors
 * topic: io_files
 * covers: unlink while open, st_nlink 0, anonymous scratch file idiom, rename over open file, hard links keep data, fork sharing an unlinked file
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

static inline const char *en(int e) {
    switch (e) {
    case 0: return "OK";
    case EEXIST: return "EEXIST";
    case ENOENT: return "ENOENT";
    case EBADF: return "EBADF";
    case EINVAL: return "EINVAL";
    case EISDIR: return "EISDIR";
    case ENOTDIR: return "ENOTDIR";
    case EAGAIN: return "EAGAIN";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EMFILE: return "EMFILE";
    case ESPIPE: return "ESPIPE";
    case EPIPE: return "EPIPE";
    case EINTR: return "EINTR";
    case EFBIG: return "EFBIG";
    case ENXIO: return "ENXIO";
    case ELOOP: return "ELOOP";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EXDEV: return "EXDEV";
    case ECHILD: return "ECHILD";
    case ESRCH: return "ESRCH";
    default: return "EOTHER";
    }
}

static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}

/* write everything, retrying short writes; 0 on success, -1 on error */
static inline int wr_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
/* read until n bytes or EOF; returns count or -1 */
static inline long rd_full(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (long)got;
}
static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
static void slurp_fd(int fd, char *out, size_t cap) {
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    long n = rd_full(fd, out, cap - 1);
    CHECK(n >= 0);
    out[n] = 0;
}

static void put(const char *name, const char *s) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && wr_all(fd, s, strlen(s)) == 0);
    close(fd);
}

int main(void) {
    char buf[128];
    struct stat st;

    /* unlink while open: name disappears, data remains */
    put("victim", "still here");
    int fd = open("victim", O_RDWR);
    CHECK(fd >= 0);
    CHECK(unlink("victim") == 0);
    errno = 0;
    printf("stat by name after unlink: %s\n", stat("victim", &st) == 0 ? "found" : en(errno));
    CHECK(fstat(fd, &st) == 0);
    printf("fstat: nlink=%d size=%ld\n", (int)st.st_nlink, (long)st.st_size);
    CHECK(st.st_nlink == 0 && st.st_size == 10);
    slurp_fd(fd, buf, sizeof buf);
    printf("read after unlink: %s\n", buf);
    CHECK(strcmp(buf, "still here") == 0);
    CHECK(pwrite(fd, "HERE", 4, 6) == 4);
    slurp_fd(fd, buf, sizeof buf);
    printf("after write: %s\n", buf);
    CHECK(strcmp(buf, "still HERE") == 0);
    CHECK(ftruncate(fd, 5) == 0 && fsize(fd) == 5);

    /* the name can be reused for an unrelated file */
    put("victim", "brand new");
    struct stat st2;
    CHECK(stat("victim", &st2) == 0 && fstat(fd, &st) == 0);
    printf("new file with the same name is a different file: %d\n", !(st.st_dev == st2.st_dev && st.st_ino == st2.st_ino));
    CHECK(!(st.st_dev == st2.st_dev && st.st_ino == st2.st_ino));
    slurp_fd(fd, buf, sizeof buf);
    CHECK(strcmp(buf, "still") == 0);
    close(fd);
    CHECK(unlink("victim") == 0);

    /* anonymous scratch: create, unlink immediately, use as temporary storage */
    fd = open("scratch", O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && unlink("scratch") == 0);
    unsigned long long sum = 0, back = 0;
    for (int i = 0; i < 200; i++) {
        unsigned v = (unsigned)xs();
        CHECK(write(fd, &v, sizeof v) == (ssize_t)sizeof v);
        sum += v;
    }
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    for (int i = 0; i < 200; i++) {
        unsigned v;
        CHECK(read(fd, &v, sizeof v) == (ssize_t)sizeof v);
        back += v;
    }
    printf("scratch file round trip: 200 values, sums equal: %d, size=%ld\n", sum == back, fsize(fd));
    CHECK(sum == back && fsize(fd) == 800);
    close(fd);

    /* rename over an open file: the old descriptor still sees the old contents */
    put("target", "old contents");
    put("newer", "replacement");
    int old_fd = open("target", O_RDONLY);
    CHECK(old_fd >= 0 && rename("newer", "target") == 0);
    slurp_fd(old_fd, buf, sizeof buf);
    printf("descriptor opened before rename sees: %s\n", buf);
    CHECK(strcmp(buf, "old contents") == 0);
    int new_fd = open("target", O_RDONLY);
    slurp_fd(new_fd, buf, sizeof buf);
    printf("fresh open sees: %s\n", buf);
    CHECK(strcmp(buf, "replacement") == 0);
    CHECK(access("newer", F_OK) != 0);
    CHECK(fstat(old_fd, &st) == 0 && st.st_nlink == 0);
    close(old_fd);
    close(new_fd);

    /* hard links keep the data alive after the original name goes */
    put("orig", "linked data");
    CHECK(link("orig", "alias") == 0 && unlink("orig") == 0);
    fd = open("alias", O_RDONLY);
    slurp_fd(fd, buf, sizeof buf);
    CHECK(fstat(fd, &st) == 0);
    printf("via surviving link: %s (nlink=%d)\n", buf, (int)st.st_nlink);
    CHECK(strcmp(buf, "linked data") == 0 && st.st_nlink == 1);
    close(fd);

    /* parent and child share an unlinked file through an inherited descriptor */
    fd = open("shared", O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && unlink("shared") == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        _exit(write(fd, "from child;", 11) == 11 ? 0 : 1);
    }
    CHECK(wait_exit(c) == 0);
    CHECK(write(fd, "from parent", 11) == 11);
    slurp_fd(fd, buf, sizeof buf);
    printf("shared unlinked file: %s\n", buf);
    CHECK(strcmp(buf, "from child;from parent") == 0);
    close(fd);

    unlink("target");
    unlink("alias");
    return 0;
}
