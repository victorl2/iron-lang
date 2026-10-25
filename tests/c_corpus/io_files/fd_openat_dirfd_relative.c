/*
 * title: openat, fstatat and friends relative to a directory fd
 * topic: io_files
 * covers: openat, fstatat, mkdirat, unlinkat, renameat, symlinkat, readlinkat, AT_FDCWD, dirfd surviving chdir and rename
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
static void put(int dfd, const char *name, const char *s) {
    int fd = openat(dfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(wr_all(fd, s, strlen(s)) == 0);
    close(fd);
}

static long size_at(int dfd, const char *name, int flags) {
    struct stat st;
    if (fstatat(dfd, name, &st, flags) != 0) return -1;
    return (long)st.st_size;
}

static void get(int dfd, const char *name, char *out, size_t cap) {
    int fd = openat(dfd, name, O_RDONLY);
    CHECK(fd >= 0);
    long n = rd_full(fd, out, cap - 1);
    CHECK(n >= 0);
    out[n] = 0;
    close(fd);
}

int main(void) {
    char buf[128];
    CHECK(mkdir("base", 0755) == 0);
    int base = open("base", O_RDONLY | O_DIRECTORY);
    CHECK(base >= 0);

    put(base, "a.txt", "alpha");
    CHECK(mkdirat(base, "sub", 0755) == 0);
    int sub = openat(base, "sub", O_RDONLY | O_DIRECTORY);
    CHECK(sub >= 0);
    put(sub, "b.txt", "bravo-bravo");
    printf("a.txt size via base: %ld\n", size_at(base, "a.txt", 0));
    printf("b.txt size via sub: %ld\n", size_at(sub, "b.txt", 0));
    printf("b.txt size via base/sub/b.txt: %ld\n", size_at(base, "sub/b.txt", 0));
    printf("a.txt seen from sub via ..: %ld\n", size_at(sub, "../a.txt", 0));
    CHECK(size_at(sub, "../a.txt", 0) == 5);

    /* the dirfd keeps working after the process changes directory */
    CHECK(mkdir("elsewhere", 0755) == 0);
    CHECK(chdir("elsewhere") == 0);
    get(base, "a.txt", buf, sizeof buf);
    printf("read a.txt after chdir: %s\n", buf);
    CHECK(strcmp(buf, "alpha") == 0);
    CHECK(fstatat(AT_FDCWD, "a.txt", &(struct stat){0}, 0) != 0 && errno == ENOENT);
    puts("AT_FDCWD lookup of a.txt: ENOENT (cwd moved)");
    CHECK(chdir("..") == 0);

    /* fchdir with a directory descriptor */
    CHECK(fchdir(sub) == 0);
    CHECK(access("b.txt", F_OK) == 0);
    puts("fchdir(sub): b.txt visible by plain name");
    CHECK(fchdir(base) == 0);
    CHECK(access("a.txt", F_OK) == 0);
    CHECK(chdir("..") == 0);

    /* rename the directory itself: the descriptor follows the inode */
    CHECK(rename("base", "renamed") == 0);
    get(base, "sub/b.txt", buf, sizeof buf);
    printf("read after renaming the directory: %s\n", buf);
    CHECK(strcmp(buf, "bravo-bravo") == 0);
    CHECK(access("base", F_OK) != 0 && access("renamed/a.txt", F_OK) == 0);

    /* renameat between two directories */
    CHECK(renameat(base, "a.txt", sub, "moved.txt") == 0);
    printf("a.txt in base after renameat: %ld\n", size_at(base, "a.txt", 0));
    printf("moved.txt in sub: %ld\n", size_at(sub, "moved.txt", 0));
    CHECK(size_at(base, "a.txt", 0) == -1 && size_at(sub, "moved.txt", 0) == 5);

    /* symlinks relative to a directory */
    CHECK(symlinkat("sub/b.txt", base, "link") == 0);
    ssize_t n = readlinkat(base, "link", buf, sizeof buf - 1);
    CHECK(n > 0);
    buf[n] = 0;
    printf("readlinkat: %s\n", buf);
    printf("follow: %ld, nofollow: %ld\n", size_at(base, "link", 0), size_at(base, "link", AT_SYMLINK_NOFOLLOW));
    CHECK(size_at(base, "link", 0) == 11 && size_at(base, "link", AT_SYMLINK_NOFOLLOW) == 9);
    errno = 0;
    int fd = openat(base, "link", O_RDONLY | O_NOFOLLOW);
    printf("openat with O_NOFOLLOW on a symlink: %s\n", fd < 0 ? (errno == ELOOP ? "ELOOP" : en(errno)) : "opened");
    CHECK(fd < 0);

    /* dirfd is ignored for absolute paths */
    char cwd[512];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    char abs[600];
    snprintf(abs, sizeof abs, "%s/renamed/sub/moved.txt", cwd);
    fd = openat(sub, abs, O_RDONLY);
    printf("openat with an absolute path ignores dirfd: %d\n", fd >= 0);
    CHECK(fd >= 0);
    close(fd);

    /* using a file descriptor as dirfd fails */
    int reg = openat(sub, "b.txt", O_RDONLY);
    CHECK(reg >= 0);
    errno = 0;
    fd = openat(reg, "x", O_RDONLY);
    printf("openat on a regular file dirfd: %s\n", en(errno));
    CHECK(fd < 0 && errno == ENOTDIR);
    errno = 0;
    fd = openat(-1 + 0 * reg, "x", O_RDONLY);
    printf("openat on invalid dirfd: %s\n", en(errno));
    CHECK(fd < 0 && errno == EBADF);
    close(reg);

    /* unlinkat with and without AT_REMOVEDIR */
    errno = 0;
    int r = unlinkat(base, "sub", 0);
    printf("unlinkat directory without AT_REMOVEDIR: %s\n", r == 0 ? "OK" : "fails");
    CHECK(r < 0);
    errno = 0;
    r = unlinkat(base, "sub", AT_REMOVEDIR);
    printf("unlinkat AT_REMOVEDIR on non-empty dir: %s\n", en(errno));
    CHECK(r < 0 && (errno == ENOTEMPTY || errno == EEXIST));
    CHECK(unlinkat(sub, "b.txt", 0) == 0 && unlinkat(sub, "moved.txt", 0) == 0);
    CHECK(unlinkat(base, "link", 0) == 0);
    CHECK(unlinkat(base, "sub", AT_REMOVEDIR) == 0);
    puts("cleaned up through unlinkat");
    close(sub);
    close(base);
    CHECK(rmdir("renamed") == 0 && rmdir("elsewhere") == 0);
    return 0;
}
