/*
 * title: Recursive tree copy preserving structure, modes and symlinks
 * topic: io_files
 * covers: recursive copy, read-only directories and files, mode restore after content copy, symlink recreation, chunked read/write copy, verification walk
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

static inline uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
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

/* whole file into a malloc'd buffer (NUL terminated); NULL on error */
static inline unsigned char *slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap + 1);
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) {
            free(b);
            close(fd);
            return NULL;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    if (len) *len = n;
    return b;
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


static long copied_bytes, copied_files, copied_dirs, copied_links;

static void copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    CHECK(in >= 0);
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(out >= 0);
    unsigned char buf[37]; /* deliberately awkward chunk size */
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        CHECK(n >= 0);
        if (n == 0) break;
        CHECK(write(out, buf, (size_t)n) == n);
        copied_bytes += n;
    }
    /* apply the final mode last: a 0444 file could not be written after chmod */
    CHECK(fchmod(out, mode) == 0);
    CHECK(close(out) == 0 && close(in) == 0);
    copied_files++;
}

static void copy_tree(const char *src, const char *dst) {
    struct stat st;
    CHECK(lstat(src, &st) == 0);
    if (S_ISLNK(st.st_mode)) {
        char t[128];
        ssize_t n = readlink(src, t, sizeof t - 1);
        CHECK(n >= 0);
        t[n] = 0;
        CHECK(symlink(t, dst) == 0);
        copied_links++;
    } else if (S_ISREG(st.st_mode)) {
        copy_file(src, dst, st.st_mode & 07777);
    } else {
        CHECK(S_ISDIR(st.st_mode));
        CHECK(mkdir(dst, 0700) == 0);
        int n;
        char **v = ls_dir(src, &n);
        CHECK(v);
        for (int i = 0; i < n; i++) {
            char *s = join2(src, v[i]), *d = join2(dst, v[i]);
            copy_tree(s, d);
            free(s);
            free(d);
        }
        ls_free(v, n);
        CHECK(chmod(dst, st.st_mode & 07777) == 0); /* 0555 dirs become read-only only now */
        copied_dirs++;
    }
}

/* number of differences between two trees (type, mode, size, content, link text, names) */
static int compare(const char *a, const char *b) {
    struct stat sa, sb;
    if (lstat(a, &sa) < 0 || lstat(b, &sb) < 0) return 1;
    if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) return 1;
    if (S_ISLNK(sa.st_mode)) {
        char x[128], y[128];
        ssize_t nx = readlink(a, x, sizeof x), ny = readlink(b, y, sizeof y);
        return !(nx == ny && nx > 0 && memcmp(x, y, (size_t)nx) == 0);
    }
    if ((sa.st_mode & 07777) != (sb.st_mode & 07777)) return 1;
    if (S_ISREG(sa.st_mode)) {
        if (sa.st_size != sb.st_size) return 1;
        size_t la, lb;
        unsigned char *x = slurp(a, &la), *y = slurp(b, &lb);
        int diff = !(x && y && la == lb && memcmp(x, y, la) == 0);
        free(x);
        free(y);
        return diff;
    }
    int na, nb, diffs = 0;
    /* directories of mode 0 or 0300 cannot be listed; not present in this tree */
    char **va = ls_dir(a, &na), **vb = ls_dir(b, &nb);
    if (!va || !vb) return 1;
    if (na != nb) diffs++;
    for (int i = 0; i < na && i < nb; i++) {
        if (strcmp(va[i], vb[i]) != 0) {
            diffs++;
            continue;
        }
        char *pa = join2(a, va[i]), *pb = join2(b, vb[i]);
        diffs += compare(pa, pb);
        free(pa);
        free(pb);
    }
    ls_free(va, na);
    ls_free(vb, nb);
    return diffs;
}

int main(void) {
    umask(022);
    mkd("src");
    mkd("src/bin");
    mkd("src/lib");
    mkd("src/lib/deep");
    mkd("src/ro");
    mkd("src/empty");
    uint32_t seed = 4242;
    static const mode_t fmodes[] = {0644, 0600, 0755, 0444, 0640};
    for (int i = 0; i < 9; i++) {
        char p[64];
        uint32_t len = rng_next(&seed) % 200;
        uint32_t mi = rng_next(&seed) % 5;
        unsigned char data[200];
        for (uint32_t j = 0; j < len; j++) data[j] = (unsigned char)('A' + rng_next(&seed) % 26);
        snprintf(p, sizeof p, "src/%s/file%d.dat", i % 3 == 0 ? "bin" : i % 3 == 1 ? "lib" : "lib/deep", i);
        CHECK(put_bytes(p, data, len) == 0);
        CHECK(chmod(p, fmodes[mi]) == 0);
    }
    put("src/ro/config", "read only tree\n");
    CHECK(chmod("src/ro/config", 0444) == 0);
    CHECK(chmod("src/ro", 0555) == 0);
    CHECK(symlink("lib/deep", "src/shortcut") == 0);
    CHECK(symlink("../missing", "src/bin/broken") == 0);
    CHECK(chmod("src/bin", 0750) == 0);
    CHECK(chmod("src/lib/deep", 0700) == 0);

    copy_tree("src", "dst");
    printf("copied: %ld dirs, %ld files, %ld symlinks, %ld bytes\n", copied_dirs, copied_files,
           copied_links, copied_bytes);
    int d = compare("src", "dst");
    printf("differences after copy: %d\n", d);
    CHECK(d == 0);
    dump_tree("dst");

    /* the checker really detects each kind of difference */
    CHECK(chmod("dst/bin", 0755) == 0);
    printf("after mode change: %d\n", compare("src", "dst"));
    CHECK(compare("src", "dst") == 1);
    CHECK(chmod("dst/bin", 0750) == 0);
    CHECK(unlink("dst/shortcut") == 0 && symlink("bin", "dst/shortcut") == 0);
    printf("after retargeting a symlink: %d\n", compare("src", "dst"));
    CHECK(compare("src", "dst") == 1);
    CHECK(rm_rf("dst/empty") == 0);
    printf("after removing a directory: detected=%d\n", compare("src", "dst") > 0);
    CHECK(compare("src", "dst") > 0);

    CHECK(rm_rf("src") == 0 && rm_rf("dst") == 0);
    return 0;
}
