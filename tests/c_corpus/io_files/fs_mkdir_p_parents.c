/*
 * title: mkdir -p with per-level error reporting
 * topic: io_files
 * covers: mkdir, EEXIST handling, ENOTDIR through a file component, umask effect on modes, chmod of final component
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

/* recursive sorted dump: kind, perms (not for symlinks), size (regular files), path */
static inline void dump_tree(const char *path) {
    int n;
    char **v = ls_dir(path, &n);
    CHECK(v != NULL);
    for (int i = 0; i < n; i++) {
        char *p = join2(path, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        char k = kind_of(st.st_mode);
        if (k == 'l') {
            char tgt[128];
            ssize_t m = readlink(p, tgt, sizeof tgt - 1);
            CHECK(m >= 0);
            tgt[m] = 0;
            printf("l %s -> %s\n", p, tgt);
        } else if (k == '-') {
            printf("- %04o %ld %s\n", (unsigned)(st.st_mode & 07777), (long)st.st_size, p);
        } else {
            printf("%c %04o %s\n", k, (unsigned)(st.st_mode & 07777), p);
            if (k == 'd') dump_tree(p);
        }
        free(p);
    }
    ls_free(v, n);
}


/* mkdir -p: parents get 0777 & ~umask, the last component gets `mode` via chmod */
static int mkdir_p(const char *path, mode_t mode) {
    char buf[256];
    size_t n = strlen(path);
    if (n == 0) {
        errno = ENOENT;
        return -1;
    }
    if (n >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0777) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0777) < 0) {
        if (errno != EEXIST) return -1;
        struct stat st;
        if (stat(buf, &st) < 0) return -1;
        if (!S_ISDIR(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
    }
    return chmod(buf, mode);
}

static const struct {
    const char *path;
    mode_t mode;
} cases[] = {
    {"a", 0755},
    {"a/b/c/d", 0750},
    {"a/b/c/d", 0700},     /* already exists: just chmod */
    {"a//b//e/", 0755},    /* doubled and trailing slashes */
    {"x/./y/../z", 0755},  /* dot components are real path components */
    {"f/g", 0755},         /* f is a regular file below */
    {"a/b/c/d/../..", 0775},
    {"", 0755},
};

int main(void) {
    umask(022);
    put("f", "plain file\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        errno = 0;
        int rc = mkdir_p(cases[i].path, cases[i].mode);
        int e = errno;
        printf("mkdir_p(\"%s\", %04o) -> %d %s\n", cases[i].path, (unsigned)cases[i].mode, rc,
               rc == 0 ? "OK" : en(e));
    }
    /* the file component blocks everything below it */
    struct stat st;
    CHECK(lstat("f", &st) == 0 && S_ISREG(st.st_mode));
    CHECK(stat("f/g", &st) < 0 && errno == ENOTDIR);
    /* x/y/../z resolved lexically by the kernel: y exists, so z lands in x */
    CHECK(stat("x/z", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat("x/y", &st) == 0);

    /* umask changes what parents get */
    umask(077);
    CHECK(mkdir_p("m/n/o", 0755) == 0);
    umask(022);
    CHECK(stat("m", &st) == 0);
    printf("parent m under umask 077: %04o\n", (unsigned)(st.st_mode & 0777));
    CHECK(stat("m/n/o", &st) == 0);
    printf("leaf m/n/o chmod 0755: %04o\n", (unsigned)(st.st_mode & 0777));
    CHECK((st.st_mode & 0777) == 0755);

    dump_tree(".");
    CHECK(rm_rf("a") == 0 && rm_rf("x") == 0 && rm_rf("m") == 0 && unlink("f") == 0);
    int n;
    char **v = ls_dir(".", &n);
    CHECK(n == 0);
    ls_free(v, n);
    return 0;
}
