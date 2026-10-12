/*
 * title: open() flag matrix with O_CREAT and O_EXCL
 * topic: io_files
 * covers: open, O_CREAT, O_EXCL, errno mapping, dangling symlink, fork race for exclusive creation
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
static void report(const char *label, int fd, int err) {
    if (fd >= 0) {
        printf("%-34s -> ok\n", label);
        close(fd);
    } else {
        printf("%-34s -> %s\n", label, en(err));
    }
}

int main(void) {
    int fd, e;

    fd = open("a.txt", O_WRONLY);
    e = errno;
    CHECK(fd < 0);
    report("O_WRONLY missing", fd, e);

    fd = open("a.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello", 5) == 5);
    report("O_CREAT|O_EXCL missing", fd, 0);

    fd = open("a.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
    e = errno;
    CHECK(fd < 0);
    report("O_CREAT|O_EXCL existing", fd, e);

    fd = open("a.txt", O_RDWR | O_CREAT, 0644);
    CHECK(fd >= 0);
    printf("O_CREAT existing keeps size        -> %ld\n", fsize(fd));
    CHECK(fsize(fd) == 5);
    close(fd);

    CHECK(symlink("nowhere", "dangling") == 0);
    fd = open("dangling", O_WRONLY | O_CREAT | O_EXCL, 0644);
    e = errno;
    CHECK(fd < 0);
    report("O_EXCL through dangling symlink", fd, e);
    struct stat st;
    CHECK(stat("nowhere", &st) < 0 && errno == ENOENT);
    puts("target of dangling link not created");

    CHECK(mkdir("d", 0755) == 0);
    fd = open("d", O_WRONLY);
    e = errno;
    CHECK(fd < 0);
    report("O_WRONLY on directory", fd, e);
    fd = open("d", O_RDONLY);
    CHECK(fd >= 0);
    report("O_RDONLY on directory", fd, 0);
    fd = open("d", O_WRONLY | O_CREAT | O_EXCL, 0644);
    e = errno;
    CHECK(fd < 0);
    report("O_CREAT|O_EXCL on directory", fd, e);

    fd = open("a.txt/x", O_WRONLY | O_CREAT, 0644);
    e = errno;
    CHECK(fd < 0);
    report("create below a regular file", fd, e);
    fd = open("nodir/x", O_WRONLY | O_CREAT, 0644);
    e = errno;
    CHECK(fd < 0);
    report("create below missing directory", fd, e);

    /* exclusive creation race: only one child can win */
    enum { KIDS = 8 };
    pid_t pids[KIDS];
    for (int i = 0; i < KIDS; i++) {
        pid_t p = fork();
        CHECK(p >= 0);
        if (p == 0) {
            int f = open("winner.lock", O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (f < 0) _exit(errno == EEXIST ? 1 : 2);
            char tag = (char)('A' + i);
            if (write(f, &tag, 1) != 1) _exit(3);
            close(f);
            _exit(0);
        }
        pids[i] = p;
    }
    int winners = 0, losers = 0, bad = 0, winner_idx = -1;
    for (int i = 0; i < KIDS; i++) {
        int c = wait_exit(pids[i]);
        if (c == 0) { winners++; winner_idx = i; }
        else if (c == 1) losers++;
        else bad++;
    }
    printf("race: winners=%d losers=%d bad=%d\n", winners, losers, bad);
    CHECK(winners == 1 && losers == KIDS - 1 && bad == 0);
    fd = open("winner.lock", O_RDONLY);
    CHECK(fd >= 0);
    char tag = 0;
    CHECK(read(fd, &tag, 1) == 1);
    CHECK(fsize(fd) == 1);
    close(fd);
    CHECK(tag == 'A' + winner_idx);
    puts("lock file holds exactly the winner's tag");

    CHECK(unlink("winner.lock") == 0);
    CHECK(unlink("a.txt") == 0);
    CHECK(unlink("dangling") == 0);
    CHECK(rmdir("d") == 0);
    return 0;
}
