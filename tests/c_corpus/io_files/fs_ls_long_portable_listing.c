/*
 * title: ls -l style listing with portable fields only
 * topic: io_files
 * covers: mode string rendering including setuid/setgid/sticky, column alignment, symlink targets, recursive listing, total of apparent sizes, pure-function table for special bits
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


/* rwxr-xr-x with s/S, s/S, t/T for the special bits */
static void mode_string(mode_t m, char out[11]) {
    out[0] = kind_of(m);
    static const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & S_ISUID) out[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID) out[6] = (m & 0010) ? 's' : 'S';
    if (m & S_ISVTX) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = 0;
}

static void list_dir(const char *dir, int recursive) {
    int n;
    char **v = ls_dir(dir, &n);
    CHECK(v);
    long total = 0;
    int width = 1;
    struct stat *sts = malloc((size_t)(n ? n : 1) * sizeof *sts);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        CHECK(lstat(p, &sts[i]) == 0);
        free(p);
        long sz = S_ISDIR(sts[i].st_mode) ? 0 : (long)sts[i].st_size;
        total += sz;
        char tmp[32];
        int w = snprintf(tmp, sizeof tmp, "%ld", sz);
        if (w > width) width = w;
    }
    printf("%s:\n", dir);
    printf("total %ld\n", total);
    for (int i = 0; i < n; i++) {
        char ms[11];
        mode_string(sts[i].st_mode, ms);
        long sz = S_ISDIR(sts[i].st_mode) ? 0 : (long)sts[i].st_size;
        if (S_ISLNK(sts[i].st_mode)) {
            /* link permissions differ by OS: show a fixed placeholder */
            char *p = join2(dir, v[i]);
            char t[128];
            ssize_t k = readlink(p, t, sizeof t - 1);
            CHECK(k >= 0);
            t[k] = 0;
            printf("l ---------- %*ld %s -> %s\n", width, sz, v[i], t);
            free(p);
        } else {
            printf("%s %*ld %s%s\n", ms, width, sz, v[i], S_ISDIR(sts[i].st_mode) ? "/" : "");
        }
    }
    if (recursive) {
        for (int i = 0; i < n; i++) {
            if (!S_ISDIR(sts[i].st_mode)) continue;
            char *p = join2(dir, v[i]);
            printf("\n");
            list_dir(p, 1);
            free(p);
        }
    }
    free(sts);
    ls_free(v, n);
}

int main(void) {
    umask(022);
    /* pure table: every combination of special bits on 755 and 644 */
    static const mode_t table[] = {
        S_IFREG | 0755, S_IFREG | 0644, S_IFDIR | 0700, S_IFREG | S_ISUID | 0755, S_IFREG | S_ISUID | 0644,
        S_IFREG | S_ISGID | 0755, S_IFREG | S_ISGID | 0745, S_IFDIR | S_ISVTX | 0777,
        S_IFDIR | S_ISVTX | 0770, S_IFREG | S_ISUID | S_ISGID | S_ISVTX | 0000,
        S_IFREG | S_ISUID | S_ISGID | S_ISVTX | 0777, S_IFIFO | 0600, S_IFLNK | 0777, S_IFCHR | 0620,
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        char ms[11];
        mode_string(table[i], ms);
        printf("%06o %s\n", (unsigned)table[i], ms);
    }
    printf("\n");

    mkd("site");
    mkd("site/assets");
    mkd("site/assets/img");
    mkd("site/private");
    put("site/index.html", "<html><body>hello</body></html>\n");
    put("site/robots.txt", "User-agent: *\n");
    put("site/run.sh", "#!/bin/sh\necho hi\n");
    CHECK(chmod("site/run.sh", 0755) == 0);
    put("site/assets/style.css", "body{margin:0}\n");
    put("site/assets/img/logo.svg", "<svg/>");
    put("site/assets/img/big.dat", "");
    CHECK(truncate("site/assets/img/big.dat", 123456) == 0);
    put("site/private/key", "secret");
    CHECK(chmod("site/private/key", 0600) == 0);
    CHECK(chmod("site/private", 0700) == 0);
    CHECK(chmod("site/assets", 0750) == 0);
    CHECK(mkfifo("site/events", 0640) == 0);
    CHECK(symlink("assets/style.css", "site/style") == 0);
    CHECK(symlink("private", "site/hidden") == 0);
    CHECK(chmod("site/robots.txt", 0444) == 0);

    list_dir("site", 1);
    CHECK(rm_rf("site") == 0);
    return 0;
}
