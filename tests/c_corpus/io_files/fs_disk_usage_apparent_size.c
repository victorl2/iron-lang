/*
 * title: du-style apparent size totals with hard link deduplication
 * topic: io_files
 * covers: recursive size aggregation, st_size (not blocks), hard links counted once via dev/ino set, symlink sizes, sparse files, per-directory rollup, cross-check with flat sum
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


typedef struct {
    dev_t dev;
    ino_t ino;
} Key;

static Key seen[64];
static int nseen;

static int first_time(const struct stat *st) {
    for (int i = 0; i < nseen; i++)
        if (seen[i].dev == st->st_dev && seen[i].ino == st->st_ino) return 0;
    CHECK(nseen < 64);
    seen[nseen].dev = st->st_dev;
    seen[nseen].ino = st->st_ino;
    nseen++;
    return 1;
}

typedef struct {
    char path[64];
    long total;
} Row;

static Row rows[32];
static int nrows;

/* returns apparent bytes below `path` (directories themselves contribute 0) */
static long du(const char *path, int dedupe) {
    int n;
    char **v = ls_dir(path, &n);
    CHECK(v);
    long sum = 0;
    for (int i = 0; i < n; i++) {
        char *p = join2(path, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode)) sum += du(p, dedupe);
        else if (S_ISREG(st.st_mode)) {
            if (!dedupe || first_time(&st)) sum += (long)st.st_size;
        } else if (S_ISLNK(st.st_mode)) sum += (long)st.st_size; /* length of the target text */
        free(p);
    }
    ls_free(v, n);
    CHECK(nrows < 32);
    snprintf(rows[nrows].path, sizeof rows[nrows].path, "%s", path);
    rows[nrows++].total = sum;
    return sum;
}

static long flat_sum(const char *root, int *counted) {
    /* independent method: explicit stack, path list, and a second dedupe pass */
    char *stack[64];
    int sp = 0;
    stack[sp++] = strdup(root);
    Key ks[64];
    int nk = 0;
    long sum = 0;
    while (sp) {
        char *dir = stack[--sp];
        int n;
        char **v = ls_dir(dir, &n);
        for (int i = 0; i < n; i++) {
            char *p = join2(dir, v[i]);
            struct stat st;
            CHECK(lstat(p, &st) == 0);
            if (S_ISDIR(st.st_mode)) {
                CHECK(sp < 64);
                stack[sp++] = p;
                continue;
            }
            int dup = 0;
            if (S_ISREG(st.st_mode))
                for (int k = 0; k < nk; k++)
                    if (ks[k].ino == st.st_ino && ks[k].dev == st.st_dev) dup = 1;
            if (!dup) {
                if (S_ISREG(st.st_mode)) {
                    ks[nk].dev = st.st_dev;
                    ks[nk++].ino = st.st_ino;
                }
                sum += (long)st.st_size;
                (*counted)++;
            }
            free(p);
        }
        ls_free(v, n);
        free(dir);
    }
    return sum;
}

static int cmp_rows(const void *a, const void *b) {
    return strcmp(((const Row *)a)->path, ((const Row *)b)->path);
}

static void fmt_size(long b, char *out, size_t cap) {
    /* du -h style with integer arithmetic, rounding up */
    if (b < 1024) snprintf(out, cap, "%ldB", b);
    else if (b < 1024 * 1024) snprintf(out, cap, "%ldK", (b + 1023) / 1024);
    else snprintf(out, cap, "%ldM", (b + 1048575) / 1048576);
}

int main(void) {
    umask(022);
    mkd("proj");
    mkd("proj/a");
    mkd("proj/a/b");
    mkd("proj/c");
    mkd("proj/empty");
    uint32_t seed = 99;
    static const char *files[] = {"proj/x.bin", "proj/a/y.bin", "proj/a/b/z.bin", "proj/c/w.bin", "proj/c/v.bin"};
    long expect_plain = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t len = 100 + rng_next(&seed) % 3000;
        unsigned char *buf = malloc(len);
        for (uint32_t j = 0; j < len; j++) buf[j] = (unsigned char)rng_next(&seed);
        CHECK(put_bytes(files[i], buf, len) == 0);
        free(buf);
        expect_plain += len;
    }
    /* hard links to two of them, symlinks, and a sparse 2 MiB file */
    CHECK(link("proj/x.bin", "proj/a/x_link.bin") == 0);
    CHECK(link("proj/x.bin", "proj/c/x_link2.bin") == 0);
    CHECK(link("proj/a/y.bin", "proj/a/b/y_link.bin") == 0);
    CHECK(symlink("../x.bin", "proj/a/sym") == 0);
    CHECK(symlink("a/b/z.bin", "proj/shortcut") == 0);
    int fd = open("proj/c/sparse.img", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && ftruncate(fd, 2 * 1048576 + 5) == 0 && close(fd) == 0);

    long links_text = (long)strlen("../x.bin") + (long)strlen("a/b/z.bin");
    long sparse = 2 * 1048576 + 5;

    nseen = 0;
    nrows = 0;
    long dedup = du("proj", 1);
    qsort(rows, (size_t)nrows, sizeof rows[0], cmp_rows);
    printf("deduplicated rollup (hard links counted once):\n");
    for (int i = 0; i < nrows; i++) {
        char h[16];
        fmt_size(rows[i].total, h, sizeof h);
        printf("%9ld %6s %s\n", rows[i].total, h, rows[i].path);
    }
    printf("expected plain=%ld links=%ld sparse=%ld => %ld\n", expect_plain, links_text, sparse,
           expect_plain + links_text + sparse);
    CHECK(dedup == expect_plain + links_text + sparse);

    nseen = 0;
    nrows = 0;
    long naive = du("proj", 0);
    long x_size = fsize("proj/x.bin"), y_size = fsize("proj/a/y.bin");
    printf("naive total (links counted each time): %ld, overcount = %ld\n", naive, naive - dedup);
    CHECK(naive - dedup == 2 * x_size + y_size);

    int counted = 0;
    long flat = flat_sum("proj", &counted);
    printf("flat sum over %d distinct entries: %ld\n", counted, flat);
    CHECK(flat == dedup);
    CHECK(rm_rf("proj") == 0);
    return 0;
}
