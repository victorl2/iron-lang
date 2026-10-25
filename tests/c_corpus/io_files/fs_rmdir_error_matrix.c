/*
 * title: rmdir and unlink error matrix
 * topic: io_files
 * covers: rmdir, unlink, ENOTEMPTY, ENOTDIR, ENOENT, symlink to directory, dot components, unlink of a directory
 * deps: libc, posix
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);       \
            exit(1);                                                   \
        }                                                              \
    } while (0)

/* errno -> stable name (never print strerror text or raw numbers) */
static inline const char *en(int e) {
    switch (e) {
    case 0: return "OK";
    case EEXIST: return "EEXIST";
    case ENOENT: return "ENOENT";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EINVAL: return "EINVAL";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case ELOOP: return "ELOOP";
    case EXDEV: return "EXDEV";
    case EBADF: return "EBADF";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    default: return "EOTHER";
    }
}

static inline int put_bytes(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            close(fd);
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return close(fd);
}

static inline void put(const char *path, const char *data) {
    CHECK(put_bytes(path, data, strlen(data)) == 0);
}

static inline void mkd(const char *path) { CHECK(mkdir(path, 0777) == 0); }

/* rm -rf through directory fds: never follows symlinks, never uses a path twice */
static inline int rm_at(int pfd, const char *name, long *count) {
    struct stat st;
    if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        if ((st.st_mode & 0700) != 0700 && fchmodat(pfd, name, 0700, 0) < 0) return -1;
        int fd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0) return -1;
        DIR *d = fdopendir(fd);
        if (!d) {
            close(fd);
            return -1;
        }
        char **names = NULL;
        size_t n = 0, cap = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                names = realloc(names, cap * sizeof *names);
            }
            names[n++] = strdup(e->d_name);
        }
        int rc = 0;
        for (size_t i = 0; i < n; i++) {
            if (rm_at(fd, names[i], count) < 0) rc = -1;
            free(names[i]);
        }
        free(names);
        closedir(d);
        if (rc < 0) return -1;
        if (unlinkat(pfd, name, AT_REMOVEDIR) < 0) return -1;
        if (count) (*count)++;
        return 0;
    }
    if (unlinkat(pfd, name, 0) < 0) return -1;
    if (count) (*count)++;
    return 0;
}

static inline int rm_rf(const char *path) { return rm_at(AT_FDCWD, path, NULL); }


/* Linux and macOS disagree on unlink(dir) (EISDIR vs EPERM) and a few others;
 * fold the platform-specific answers into one class name. */
static const char *cls(int e) {
    if (e == EISDIR || e == EPERM) return "EISDIR_OR_EPERM";
    if (e == EEXIST || e == ENOTEMPTY) return "ENOTEMPTY_OR_EEXIST";
    return en(e);
}

static void try_rmdir(const char *label, const char *path) {
    errno = 0;
    int rc = rmdir(path);
    int e = errno;
    printf("rmdir %-14s -> %s\n", label, rc == 0 ? "OK" : cls(e));
}

static void try_unlink(const char *label, const char *path) {
    errno = 0;
    int rc = unlink(path);
    int e = errno;
    printf("unlink %-13s -> %s\n", label, rc == 0 ? "OK" : cls(e));
}

int main(void) {
    umask(022);
    mkd("empty");
    mkd("full");
    put("full/file", "x");
    mkd("full/sub");
    put("plain", "data");
    CHECK(symlink("empty", "lnk_dir") == 0);
    CHECK(symlink("plain", "lnk_file") == 0);
    CHECK(symlink("nowhere", "lnk_dangling") == 0);

    try_rmdir("missing", "missing");
    try_rmdir("regular file", "plain");
    try_rmdir("non-empty", "full");
    try_rmdir("dot", "empty/.");
    try_rmdir("symlink->dir", "lnk_dir");
    try_rmdir("file/", "plain/");
    try_rmdir("through file", "plain/x");
    try_rmdir("empty", "empty");
    try_rmdir("empty again", "empty");

    try_unlink("directory", "full");
    try_unlink("missing", "missing");
    try_unlink("dir symlink", "lnk_dir");
    try_unlink("file symlink", "lnk_file");
    try_unlink("dangling", "lnk_dangling");
    try_unlink("through file", "plain/x");

    /* the symlink removal must not touch targets */
    struct stat st;
    CHECK(stat("plain", &st) == 0 && st.st_size == 4);
    CHECK(lstat("empty", &st) < 0 && errno == ENOENT);

    /* peel a tree bottom-up by hand */
    CHECK(unlink("full/file") == 0);
    try_rmdir("full (sub left)", "full");
    CHECK(rmdir("full/sub") == 0);
    try_rmdir("full (emptied)", "full");

    CHECK(rm_rf("plain") == 0 && rm_rf("lnk_file") == 0 && rm_rf("lnk_dangling") == 0);
    CHECK(rm_rf("lnk_dir") == 0);
    CHECK(rm_rf("full") == 0);
    /* (rmdir on "symlink/" is deliberately not probed: macOS follows it, Linux says ENOTDIR) */
    return 0;
}
