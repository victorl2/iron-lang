/*
 * title: Dangling symlinks: detect, repair by creating the target, break again
 * topic: io_files
 * covers: symlink, lstat vs stat, ENOENT through a dangling link, O_CREAT through a dangling link, open O_EXCL refusing symlink, relative vs absolute-style targets, rename of the target
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

static inline long fsize(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    return (long)st.st_size;
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

static inline int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sorted names in a directory, without "." and ".."; NULL on error */
static inline char **ls_dir(const char *path, int *count) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    char **v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            v = realloc(v, (size_t)cap * sizeof *v);
        }
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n > 1) qsort(v, (size_t)n, sizeof *v, cmp_str);
    if (!v) v = malloc(sizeof *v);
    *count = n;
    return v;
}

static inline void ls_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static inline char *join2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb + 1);
    return r;
}


typedef enum { HEALTHY, DANGLING, OTHER } Health;

static Health health(const char *path, const char **what) {
    struct stat l, s;
    if (lstat(path, &l) < 0) {
        *what = "missing";
        return OTHER;
    }
    if (!S_ISLNK(l.st_mode)) {
        *what = "not a link";
        return OTHER;
    }
    if (stat(path, &s) == 0) {
        *what = S_ISDIR(s.st_mode) ? "dir target" : "file target";
        return HEALTHY;
    }
    CHECK(errno == ENOENT);
    *what = "dangling";
    return DANGLING;
}

static void report(const char *dir) {
    int n;
    char **v = ls_dir(dir, &n);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        const char *w;
        Health h = health(p, &w);
        char t[64] = "";
        if (h != OTHER) {
            ssize_t k = readlink(p, t, sizeof t - 1);
            CHECK(k >= 0);
            t[k] = 0;
        }
        printf("  %-8s -> %-14s %s\n", v[i], t[0] ? t : "-", w);
        free(p);
    }
    ls_free(v, n);
}

int main(void) {
    umask(022);
    mkd("w");
    mkd("w/data");
    put("w/data/real.txt", "real");
    CHECK(symlink("data/real.txt", "w/ok") == 0);
    CHECK(symlink("data/later.txt", "w/soon") == 0);
    CHECK(symlink("data", "w/dirlink") == 0);
    CHECK(symlink("nowhere/deeper", "w/nope") == 0);
    put("w/plain", "p");

    printf("initial:\n");
    report("w");

    /* opening a dangling link for reading fails, for creation it creates the target */
    int fd = open("w/soon", O_RDONLY);
    CHECK(fd < 0);
    printf("open(soon, RDONLY): %s\n", en(errno));
    fd = open("w/soon", O_WRONLY | O_CREAT | O_EXCL, 0644);
    printf("open(soon, CREAT|EXCL): %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd < 0 && errno == EEXIST); /* O_EXCL never follows a symlink, even a dangling one */

    fd = open("w/soon", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello", 5) == 5);
    CHECK(close(fd) == 0);
    printf("open(soon, CREAT): created target data/later.txt size=%ld\n", fsize("w/data/later.txt"));

    /* creating through a link whose parent is missing fails */
    errno = 0;
    fd = open("w/nope", O_WRONLY | O_CREAT, 0644);
    CHECK(fd < 0);
    printf("open(nope, CREAT): %s\n", en(errno));

    printf("after repair:\n");
    report("w");

    /* renaming the target breaks the link again; the link text is unchanged */
    CHECK(rename("w/data/real.txt", "w/data/moved.txt") == 0);
    printf("after renaming real.txt:\n");
    report("w");

    /* deleting the link never touches the target, deleting a target never touches links */
    CHECK(unlink("w/ok") == 0);
    CHECK(unlink("w/data/later.txt") == 0);
    printf("after unlink(ok) and unlink(later.txt):\n");
    report("w");
    CHECK(fsize("w/data/moved.txt") == 4);

    /* dir links are healthy until the directory goes away */
    CHECK(rm_rf("w/data") == 0);
    printf("after removing data/:\n");
    report("w");

    /* count dangling links */
    int n, dang = 0;
    char **v = ls_dir("w", &n);
    for (int i = 0; i < n; i++) {
        char *p = join2("w", v[i]);
        const char *w;
        if (health(p, &w) == DANGLING) dang++;
        free(p);
    }
    ls_free(v, n);
    printf("dangling links: %d of %d entries\n", dang, n);
    CHECK(dang == 3);
    CHECK(rm_rf("w") == 0);
    return 0;
}
