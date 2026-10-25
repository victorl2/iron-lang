/*
 * title: stat vs lstat over every file type
 * topic: io_files
 * covers: stat, lstat, fstatat AT_SYMLINK_NOFOLLOW, S_IS* macros, fifo, unix socket file, character device, sparse file size, dangling symlink
 * deps: libc, posix, sockets
 */
#include <sys/socket.h>
#include <sys/un.h>
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

static inline char kind_of(mode_t m) {
    if (S_ISREG(m)) return '-';
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    if (S_ISCHR(m)) return 'c';
    if (S_ISBLK(m)) return 'b';
    return '?';
}

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


static void row(const char *label, const char *path) {
    struct stat a, b;
    int r1 = lstat(path, &a);
    int e1 = errno;
    int r2 = stat(path, &b);
    int e2 = errno;
    printf("%-10s lstat:", label);
    if (r1 < 0) printf(" %s", en(e1));
    else if (S_ISREG(a.st_mode) || S_ISLNK(a.st_mode)) printf(" %c size=%ld", kind_of(a.st_mode), (long)a.st_size);
    else printf(" %c", kind_of(a.st_mode));
    printf("  stat:");
    if (r2 < 0) printf(" %s", en(e2));
    else if (S_ISREG(b.st_mode)) printf(" %c size=%ld", kind_of(b.st_mode), (long)b.st_size);
    else printf(" %c", kind_of(b.st_mode));
    printf("\n");

    /* fstatat(AT_SYMLINK_NOFOLLOW) must agree with lstat, fstatat(0) with stat */
    struct stat c, d;
    int r3 = fstatat(AT_FDCWD, path, &c, AT_SYMLINK_NOFOLLOW);
    int r4 = fstatat(AT_FDCWD, path, &d, 0);
    CHECK((r1 == 0) == (r3 == 0) && (r2 == 0) == (r4 == 0));
    if (r1 == 0) CHECK(a.st_mode == c.st_mode && a.st_size == c.st_size);
    if (r2 == 0) CHECK(b.st_mode == d.st_mode && b.st_size == d.st_size);
}

int main(void) {
    umask(022);
    put("reg", "hello, world\n");
    put("empty", "");
    mkd("dir");
    CHECK(mkfifo("fifo", 0644) == 0);
    CHECK(symlink("reg", "sl_file") == 0);
    CHECK(symlink("dir", "sl_dir") == 0);
    CHECK(symlink("nothing", "sl_dead") == 0);
    CHECK(symlink("sl_file", "sl_chain") == 0);
    CHECK(link("reg", "hard") == 0);

    /* sparse file: apparent size only, never blocks */
    int fd = open("sparse", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 1048575, SEEK_SET) == 1048575);
    CHECK(write(fd, "Z", 1) == 1);
    CHECK(close(fd) == 0);

    /* a unix socket file bound at a relative path */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(s >= 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "sock");
    CHECK(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0);

    static const char *rows[][2] = {
        {"regular", "reg"},     {"empty", "empty"},    {"hardlink", "hard"},   {"sparse", "sparse"},
        {"directory", "dir"},   {"fifo", "fifo"},      {"socket", "sock"},     {"sl->file", "sl_file"},
        {"sl->dir", "sl_dir"},  {"sl->dead", "sl_dead"}, {"sl chain", "sl_chain"},
        {"chardev", "/dev/null"}, {"missing", "absent"}, {"via file", "reg/x"},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) row(rows[i][0], rows[i][1]);

    /* trailing slash forces directory semantics */
    struct stat st;
    errno = 0;
    int rc = stat("reg/", &st);
    printf("stat(\"reg/\"): %s\n", rc == 0 ? "OK" : en(errno));
    CHECK(lstat("sl_dir/", &st) == 0 && S_ISDIR(st.st_mode));
    printf("lstat(\"sl_dir/\") reports directory: yes\n");

    /* hard links share identity */
    struct stat a, b;
    CHECK(stat("reg", &a) == 0 && stat("hard", &b) == 0);
    CHECK(a.st_ino == b.st_ino && a.st_dev == b.st_dev);
    printf("reg and hard are the same file, nlink=%ld\n", (long)a.st_nlink);

    /* permission bits of the things we created (umask 022) */
    CHECK(stat("fifo", &st) == 0);
    printf("fifo mode %04o, reg mode %04o\n", (unsigned)(st.st_mode & 07777), (unsigned)(a.st_mode & 07777));

    close(s);
    static const char *all[] = {"reg", "empty", "dir", "fifo", "sl_file", "sl_dir", "sl_dead",
                                "sl_chain", "hard", "sparse", "sock"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) CHECK(rm_rf(all[i]) == 0);
    return 0;
}
