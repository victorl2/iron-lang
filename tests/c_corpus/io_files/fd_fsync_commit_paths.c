/*
 * title: fsync call paths and atomic replace protocol
 * topic: io_files
 * covers: fsync on files, directories, read-only descriptors, pipes, rename-based atomic update, crash simulation with fork
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
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
static void slurp(const char *name, char *out, size_t cap) {
    int fd = open(name, O_RDONLY);
    CHECK(fd >= 0);
    long n = rd_full(fd, out, cap - 1);
    CHECK(n >= 0);
    out[n] = 0;
    close(fd);
}

/*
 * Replace `name` with `content` without ever exposing a partial file:
 * write a temporary sibling, fsync it, rename over the target, fsync the directory.
 */
static int atomic_replace(const char *dir, const char *name, const char *content, int *fsyncs) {
    char tmp[64], path[64];
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", dir, name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (wr_all(fd, content, strlen(content)) != 0) { close(fd); return -1; }
    if (fsync(fd) != 0) { close(fd); return -1; }
    (*fsyncs)++;
    close(fd);
    if (rename(tmp, path) != 0) return -1;
    int dfd = open(dir, O_RDONLY);
    if (dfd < 0) return -1;
    int rc = fsync(dfd);
    if (rc == 0) (*fsyncs)++;
    close(dfd);
    return rc;
}

int main(void) {
    int fd = open("data.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(wr_all(fd, "payload", 7) == 0);
    int r = fsync(fd);
    printf("fsync regular file (rdwr): %s\n", en(r == 0 ? 0 : errno));
    CHECK(r == 0);
    close(fd);

    fd = open("data.bin", O_RDONLY);
    CHECK(fd >= 0);
    r = fsync(fd);
    printf("fsync regular file (rdonly): %s\n", en(r == 0 ? 0 : errno));
    CHECK(r == 0);
    close(fd);

    CHECK(mkdir("store", 0755) == 0);
    fd = open("store", O_RDONLY);
    CHECK(fd >= 0);
    r = fsync(fd);
    printf("fsync directory: %s\n", en(r == 0 ? 0 : errno));
    CHECK(r == 0);
    close(fd);

    errno = 0;
    r = fsync(fd);
    printf("fsync closed descriptor: %s\n", en(errno));
    CHECK(r < 0 && errno == EBADF);

    int p[2];
    CHECK(pipe(p) == 0);
    errno = 0;
    r = fsync(p[1]);
    int e = errno;
    /* pipes cannot be synced; the exact errno is EINVAL on the systems we target */
    printf("fsync pipe: %s\n", r == 0 ? "OK" : (e == EINVAL ? "EINVAL" : "error"));
    CHECK(r < 0);
    close(p[0]);
    close(p[1]);

    /* data is visible to other readers before any sync (shared page cache) */
    fd = open("visible.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(wr_all(fd, "unsynced", 8) == 0);
    char buf[64];
    slurp("visible.txt", buf, sizeof buf);
    printf("other reader sees before fsync: %s\n", buf);
    CHECK(strcmp(buf, "unsynced") == 0);
    close(fd);

    /* a child writes and exits without fsync or close: data is still there */
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        int f = open("crashy.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0 || write(f, "left behind", 11) != 11) _exit(2);
        _exit(0);
    }
    CHECK(wait_exit(c) == 0);
    slurp("crashy.txt", buf, sizeof buf);
    printf("after child _exit without fsync: %s\n", buf);
    CHECK(strcmp(buf, "left behind") == 0);

    /* atomic replace: readers see either the old or the new whole content, never a mix */
    int syncs = 0;
    CHECK(atomic_replace("store", "config", "version=1\n", &syncs) == 0);
    slurp("store/config", buf, sizeof buf);
    printf("config v1: %s", buf);
    for (int v = 2; v <= 6; v++) {
        char content[64];
        snprintf(content, sizeof content, "version=%d\nsize=%d\n", v, v * 100);
        CHECK(atomic_replace("store", "config", content, &syncs) == 0);
        slurp("store/config", buf, sizeof buf);
        CHECK(strcmp(buf, content) == 0);
    }
    printf("config v6: %s", buf);
    printf("fsync calls in protocol: %d\n", syncs);
    CHECK(syncs == 12);

    /* the temporary name never lingers */
    struct stat st;
    errno = 0;
    r = stat("store/.config.tmp", &st);
    printf("temp file after replace: %s\n", r == 0 ? "present" : en(errno));
    CHECK(r < 0 && errno == ENOENT);

    /* an interrupted update leaves the old version intact */
    pid_t k = fork();
    CHECK(k >= 0);
    if (k == 0) {
        int f = open("store/.config.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) _exit(2);
        if (write(f, "version=99 half wri", 19) != 19) _exit(3);
        _exit(9); /* dies before rename */
    }
    CHECK(wait_exit(k) == 9);
    slurp("store/config", buf, sizeof buf);
    printf("config after interrupted update: %.9s\n", buf);
    CHECK(strncmp(buf, "version=6", 9) == 0);
    CHECK(unlink("store/.config.tmp") == 0);

    CHECK(unlink("store/config") == 0);
    CHECK(rmdir("store") == 0);
    unlink("data.bin");
    unlink("visible.txt");
    unlink("crashy.txt");
    return 0;
}
