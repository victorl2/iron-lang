/*
 * title: umask applied to open, mkdir, mkfifo and mkdirat
 * topic: io_files
 * covers: umask, mode & ~umask for files, directories, fifos, existing-file O_CREAT keeps mode, chmod ignores umask, umask return value, read-only query idiom
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


static unsigned mode_of(const char *p) {
    struct stat st;
    CHECK(lstat(p, &st) == 0);
    return (unsigned)(st.st_mode & 0777);
}

static void mk(const char *path, char kind, mode_t req) {
    switch (kind) {
    case 'f': {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, req);
        CHECK(fd >= 0);
        CHECK(close(fd) == 0);
        break;
    }
    case 'd': CHECK(mkdir(path, req) == 0); break;
    case 'p': CHECK(mkfifo(path, req) == 0); break;
    default: CHECK(0);
    }
}

int main(void) {
    static const mode_t masks[] = {000, 022, 027, 077, 002, 0222, 0777};
    static const struct {
        char kind;
        mode_t req;
    } reqs[] = {{'f', 0666}, {'f', 0600}, {'f', 0777}, {'d', 0777}, {'d', 0755}, {'p', 0666}};

    for (size_t mi = 0; mi < sizeof masks / sizeof masks[0]; mi++) {
        mode_t old = umask(masks[mi]);
        (void)old;
        printf("umask %03o:", (unsigned)masks[mi]);
        for (size_t ri = 0; ri < sizeof reqs / sizeof reqs[0]; ri++) {
            char p[32];
            snprintf(p, sizeof p, "e_%zu_%zu", mi, ri);
            mk(p, reqs[ri].kind, reqs[ri].req);
            unsigned got = mode_of(p);
            unsigned want = (unsigned)(reqs[ri].req & ~masks[mi] & 0777);
            CHECK(got == want);
            printf(" %c%03o>%03o", reqs[ri].kind, (unsigned)reqs[ri].req, got);
        }
        printf("\n");
    }

    /* umask returns the previous value; the query idiom restores it */
    umask(027);
    mode_t q = umask(0);
    umask(q);
    mode_t q2 = umask(q);
    printf("query idiom sees %03o then %03o\n", (unsigned)q, (unsigned)q2);
    CHECK(q == 027 && q2 == 027);

    /* O_CREAT on an existing file leaves its mode alone; chmod ignores the mask */
    umask(022);
    put("keep", "k");
    CHECK(chmod("keep", 0640) == 0);
    int fd = open("keep", O_WRONLY | O_CREAT, 0777);
    CHECK(fd >= 0);
    close(fd);
    printf("O_CREAT on existing file keeps: %03o\n", mode_of("keep"));
    umask(077);
    CHECK(chmod("keep", 0664) == 0);
    printf("chmod 664 under umask 077: %03o\n", mode_of("keep"));
    CHECK(mode_of("keep") == 0664);

    /* mkdirat and openat honour it too */
    umask(037);
    int dfd = open(".", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    CHECK(mkdirat(dfd, "at_dir", 0777) == 0);
    int fd2 = openat(dfd, "at_file", O_WRONLY | O_CREAT | O_EXCL, 0666);
    CHECK(fd2 >= 0);
    close(fd2);
    close(dfd);
    printf("under umask 037: mkdirat 777 -> %03o, openat 666 -> %03o\n", mode_of("at_dir"),
           mode_of("at_file"));

    umask(022);
    /* a directory created 0 is unusable until chmod: creation inside fails (unless privileged) */
    CHECK(mkdir("zero", 0) == 0);
    CHECK(mode_of("zero") == 0);
    CHECK(chmod("zero", 0755) == 0);
    printf("mkdir mode 0 then chmod 755: %03o\n", mode_of("zero"));

    DIR *d = opendir(".");
    CHECK(d);
    struct dirent *e;
    int created = 0;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') created++;
    closedir(d);
    printf("entries created: %d\n", created);
    d = opendir(".");
    char **names = NULL;
    size_t n = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        names = realloc(names, (n + 1) * sizeof *names);
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (size_t i = 0; i < n; i++) {
        CHECK(rm_rf(names[i]) == 0);
        free(names[i]);
    }
    free(names);
    return 0;
}
