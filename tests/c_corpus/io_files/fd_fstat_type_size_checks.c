/*
 * title: fstat type, size, mode and identity checks
 * topic: io_files
 * covers: fstat, lstat, S_ISREG/S_ISDIR/S_ISFIFO/S_ISCHR/S_ISSOCK/S_ISLNK, st_size, st_nlink, fchmod, dev/ino identity
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
#include <sys/socket.h>

static const char *kind(mode_t m) {
    if (S_ISREG(m)) return "regular";
    if (S_ISDIR(m)) return "directory";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISCHR(m)) return "chardev";
    if (S_ISSOCK(m)) return "socket";
    if (S_ISLNK(m)) return "symlink";
    return "other";
}

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

int main(void) {
    struct stat st, st2;

    int f = open("plain", O_RDWR | O_CREAT | O_TRUNC, 0640);
    CHECK(f >= 0 && fstat(f, &st) == 0);
    printf("plain file: %s size=%ld nlink=%d\n", kind(st.st_mode), (long)st.st_size, (int)st.st_nlink);
    CHECK(S_ISREG(st.st_mode) && st.st_size == 0 && st.st_nlink == 1);

    char blob[3000];
    memset(blob, 'q', sizeof blob);
    CHECK(wr_all(f, blob, sizeof blob) == 0);
    CHECK(fstat(f, &st) == 0);
    printf("after writing 3000 bytes: size=%ld\n", (long)st.st_size);
    CHECK(st.st_size == 3000);
    CHECK(ftruncate(f, 10) == 0 && fstat(f, &st) == 0);
    printf("after ftruncate to 10: size=%ld\n", (long)st.st_size);
    CHECK(st.st_size == 10);

    /* hard link: same identity, link count 2 */
    CHECK(link("plain", "plain2") == 0);
    CHECK(fstat(f, &st) == 0 && stat("plain2", &st2) == 0);
    printf("after link(): nlink=%d same file=%d\n", (int)st.st_nlink, same_file(&st, &st2));
    CHECK(st.st_nlink == 2 && same_file(&st, &st2));
    CHECK(unlink("plain2") == 0 && fstat(f, &st) == 0);
    printf("after unlinking the link: nlink=%d\n", (int)st.st_nlink);
    CHECK(st.st_nlink == 1);

    /* identity through dup and through a second open */
    int d = dup(f), o = open("plain", O_RDONLY);
    CHECK(d >= 0 && o >= 0);
    struct stat sd, so;
    CHECK(fstat(d, &sd) == 0 && fstat(o, &so) == 0);
    printf("dup and second open name the same file: %d %d\n", same_file(&st, &sd), same_file(&st, &so));
    CHECK(same_file(&st, &sd) && same_file(&st, &so));
    close(d);
    close(o);

    /* permission bits: creation mode is filtered by umask, fchmod sets exactly */
    CHECK(fchmod(f, 0600) == 0 && fstat(f, &st) == 0);
    printf("mode after fchmod 0600: %03o\n", (unsigned)(st.st_mode & 0777));
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(fchmod(f, 0444) == 0 && fstat(f, &st) == 0);
    printf("mode after fchmod 0444: %03o\n", (unsigned)(st.st_mode & 0777));
    CHECK((st.st_mode & 0777) == 0444);
    close(f);

    CHECK(mkdir("dir", 0755) == 0);
    int df = open("dir", O_RDONLY);
    CHECK(df >= 0 && fstat(df, &st) == 0);
    printf("directory: %s\n", kind(st.st_mode));
    CHECK(S_ISDIR(st.st_mode));
    close(df);

    int p[2];
    CHECK(pipe(p) == 0);
    CHECK(fstat(p[0], &st) == 0);
    printf("pipe read end: %s\n", kind(st.st_mode));
    CHECK(S_ISFIFO(st.st_mode));
    CHECK(fstat(p[1], &st2) == 0 && S_ISFIFO(st2.st_mode));
    close(p[0]);
    close(p[1]);

    CHECK(mkfifo("named", 0600) == 0);
    CHECK(stat("named", &st) == 0);
    printf("named fifo: %s\n", kind(st.st_mode));
    CHECK(S_ISFIFO(st.st_mode));
    unlink("named");

    int nul = open("/dev/null", O_RDWR);
    CHECK(nul >= 0 && fstat(nul, &st) == 0);
    printf("/dev/null: %s size=%ld\n", kind(st.st_mode), (long)st.st_size);
    CHECK(S_ISCHR(st.st_mode) && st.st_size == 0);
    char big[100];
    memset(big, 1, sizeof big);
    CHECK(write(nul, big, sizeof big) == 100);
    CHECK(read(nul, big, sizeof big) == 0);
    puts("/dev/null swallows writes and reads EOF");
    close(nul);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(fstat(sv[0], &st) == 0);
    printf("socketpair end: %s\n", kind(st.st_mode));
    CHECK(S_ISSOCK(st.st_mode));
    close(sv[0]);
    close(sv[1]);

    /* symlink: lstat sees the link (size = target length), stat follows it */
    CHECK(symlink("plain", "ln") == 0);
    CHECK(lstat("ln", &st) == 0 && stat("ln", &st2) == 0);
    printf("lstat: %s size=%ld; stat: %s size=%ld\n", kind(st.st_mode), (long)st.st_size, kind(st2.st_mode), (long)st2.st_size);
    CHECK(S_ISLNK(st.st_mode) && st.st_size == 5 && S_ISREG(st2.st_mode) && st2.st_size == 10);
    CHECK(unlink("ln") == 0);
    CHECK(symlink("missing-target", "dangling") == 0);
    errno = 0;
    int r = stat("dangling", &st);
    int e = errno;
    printf("stat dangling: %d %s; lstat ok: %d\n", r, en(e), lstat("dangling", &st) == 0);
    CHECK(r < 0 && e == ENOENT);
    unlink("dangling");

    /* fstat on a closed descriptor */
    errno = 0;
    r = fstat(f, &st);
    printf("fstat closed fd: %d %s\n", r, en(errno));
    CHECK(r < 0 && errno == EBADF);

    chmod("plain", 0644);
    unlink("plain");
    rmdir("dir");
    return 0;
}
