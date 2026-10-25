/*
 * title: Hard-link snapshot farm (rsync --link-dest style) with link count accounting
 * topic: io_files
 * covers: link vs copy per file against the previous snapshot, st_nlink accounting, logical vs physical bytes via distinct inodes, deleting the oldest snapshot, tree hashes stable after pruning, shared-inode write hazard
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


static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* hash of a tree: sorted relative paths, types, and file contents */
static uint64_t tree_hash(const char *dir, const char *rel, uint64_t h) {
    int n;
    char **v = ls_dir(dir, &n);
    CHECK(v);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        char r[128];
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        h = fnv(h, r, strlen(r));
        if (S_ISDIR(st.st_mode)) {
            h = fnv(h, "D", 1);
            h = tree_hash(p, r, h);
        } else {
            size_t len;
            unsigned char *b = slurp(p, &len);
            CHECK(b);
            h = fnv(h, "F", 1);
            h = fnv(h, b, len);
            free(b);
        }
        free(p);
    }
    ls_free(v, n);
    return h;
}

typedef struct {
    long files, linked, copied, logical;
} SnapStat;

static int same_content(const char *a, const char *b) {
    size_t la, lb;
    unsigned char *x = slurp(a, &la), *y = slurp(b, &lb);
    int r = x && y && la == lb && !memcmp(x, y, la);
    free(x);
    free(y);
    return r;
}

/* copy work/ into snap/, hard-linking files unchanged relative to prev/ (prev may be NULL) */
static void snapshot_dir(const char *work, const char *prev, const char *snap, SnapStat *s) {
    CHECK(mkdir(snap, 0755) == 0);
    int n;
    char **v = ls_dir(work, &n);
    for (int i = 0; i < n; i++) {
        char *w = join2(work, v[i]), *d = join2(snap, v[i]);
        char *p = prev ? join2(prev, v[i]) : NULL;
        struct stat st;
        CHECK(lstat(w, &st) == 0);
        if (S_ISDIR(st.st_mode)) {
            struct stat ps;
            snapshot_dir(w, p && lstat(p, &ps) == 0 && S_ISDIR(ps.st_mode) ? p : NULL, d, s);
        } else {
            s->files++;
            s->logical += (long)st.st_size;
            struct stat ps;
            if (p && lstat(p, &ps) == 0 && S_ISREG(ps.st_mode) && ps.st_size == st.st_size && same_content(w, p)) {
                CHECK(link(p, d) == 0);
                s->linked++;
            } else {
                size_t len;
                unsigned char *b = slurp(w, &len);
                CHECK(b && put_bytes(d, b, len) == 0);
                free(b);
                s->copied++;
            }
        }
        free(w);
        free(d);
        free(p);
    }
    ls_free(v, n);
}

/* distinct inodes below a set of snapshots: physical bytes */
typedef struct {
    dev_t dev;
    ino_t ino;
} Key;

static Key keys[256];
static int nkeys;
static long physical;

static void phys_walk(const char *dir) {
    int n;
    char **v = ls_dir(dir, &n);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode)) phys_walk(p);
        else {
            int seen = 0;
            for (int k = 0; k < nkeys; k++)
                if (keys[k].dev == st.st_dev && keys[k].ino == st.st_ino) seen = 1;
            if (!seen) {
                CHECK(nkeys < 256);
                keys[nkeys].dev = st.st_dev;
                keys[nkeys++].ino = st.st_ino;
                physical += (long)st.st_size;
            }
        }
        free(p);
    }
    ls_free(v, n);
}

static void fill(const char *path, uint32_t *seed, int len) {
    char buf[200];
    CHECK(len < 200);
    for (int i = 0; i < len; i++) buf[i] = (char)('a' + rng_next(seed) % 26);
    CHECK(put_bytes(path, buf, (size_t)len) == 0);
}

int main(void) {
    umask(022);
    uint32_t seed = 555;
    mkd("work");
    mkd("work/docs");
    mkd("work/src");
    mkd("snaps");
    char path[64];
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof path, "work/src/f%d.c", i);
        fill(path, &seed, 40 + 10 * i);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "work/docs/d%d.md", i);
        fill(path, &seed, 100 + i);
    }
    fill("work/top.txt", &seed, 33);
    fill("work/big.dat", &seed, 190);

    static const char *snames[4] = {"snaps/s0", "snaps/s1", "snaps/s2", "snaps/s3"};
    uint64_t hashes[4];
    for (int s = 0; s < 4; s++) {
        if (s == 1) {
            fill("work/src/f1.c", &seed, 45);      /* changed */
            fill("work/docs/d2.md", &seed, 104);   /* changed */
            fill("work/docs/new1.md", &seed, 20);  /* added */
            fill("work/src/new2.c", &seed, 25);    /* added */
            CHECK(unlink("work/top.txt") == 0);    /* deleted */
        } else if (s == 2) {
            fill("work/big.dat", &seed, 191);      /* changed */
            CHECK(rename("work/src/f3.c", "work/src/f3_renamed.c") == 0); /* same bytes, new path */
        }
        SnapStat st = {0, 0, 0, 0};
        snapshot_dir("work", s ? snames[s - 1] : NULL, snames[s], &st);
        hashes[s] = tree_hash(snames[s], "", 1469598103934665603ull);
        printf("s%d: files=%ld linked=%ld copied=%ld logical=%ld bytes\n", s, st.files, st.linked, st.copied, st.logical);
    }

    /* accounting across all snapshots */
    nkeys = 0;
    physical = 0;
    phys_walk("snaps");
    long logical_total = 0;
    /* recompute logical sum by walking each snapshot separately */
    for (int s = 0; s < 4; s++) {
        int n;
        char **v = ls_dir(snames[s], &n);
        for (int i = 0; i < n; i++) {
            char *p = join2(snames[s], v[i]);
            struct stat stt;
            CHECK(lstat(p, &stt) == 0);
            if (S_ISDIR(stt.st_mode)) {
                int m;
                char **w = ls_dir(p, &m);
                for (int j = 0; j < m; j++) {
                    char *q = join2(p, w[j]);
                    logical_total += fsize(q);
                    free(q);
                }
                ls_free(w, m);
            } else logical_total += (long)stt.st_size;
            free(p);
        }
        ls_free(v, n);
    }
    printf("logical bytes over 4 snapshots: %ld, physical bytes (distinct inodes): %ld\n", logical_total, physical);
    CHECK(physical < logical_total);

    /* link counts: a file untouched throughout is shared by all four snapshots */
    struct stat a;
    CHECK(stat("snaps/s3/src/f0.c", &a) == 0);
    printf("f0.c untouched: nlink=%ld\n", (long)a.st_nlink);
    CHECK(a.st_nlink == 4);
    CHECK(stat("snaps/s3/src/f1.c", &a) == 0);
    printf("f1.c changed once (s1): nlink=%ld\n", (long)a.st_nlink);
    CHECK(a.st_nlink == 3);
    CHECK(stat("snaps/s3/src/f3_renamed.c", &a) == 0);
    printf("f3_renamed.c copied at s2 (new path): nlink=%ld\n", (long)a.st_nlink);
    CHECK(a.st_nlink == 2);
    CHECK(stat("snaps/s0/top.txt", &a) == 0);
    printf("top.txt only in s0: nlink=%ld\n", (long)a.st_nlink);
    CHECK(a.st_nlink == 1);

    /* prune the oldest snapshot: shared data survives, the others still hash the same */
    CHECK(rm_rf("snaps/s0") == 0);
    CHECK(stat("snaps/s3/src/f0.c", &a) == 0);
    printf("after deleting s0: f0.c nlink=%ld\n", (long)a.st_nlink);
    CHECK(a.st_nlink == 3);
    for (int s = 1; s < 4; s++) {
        uint64_t h = tree_hash(snames[s], "", 1469598103934665603ull);
        CHECK(h == hashes[s]);
    }
    printf("s1..s3 tree hashes unchanged after pruning s0: yes\n");
    nkeys = 0;
    physical = 0;
    phys_walk("snaps");
    printf("physical bytes now: %ld\n", physical);

    /* the hazard: writing through one name of a shared inode edits every snapshot that links it */
    int fd = open("snaps/s3/src/f0.c", O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "!", 1) == 1 && close(fd) == 0);
    uint64_t h1 = tree_hash(snames[1], "", 1469598103934665603ull);
    printf("in-place write to a linked file changed s1 as well: %s\n", h1 != hashes[1] ? "yes" : "no");
    CHECK(h1 != hashes[1]);

    CHECK(rm_rf("work") == 0 && rm_rf("snaps") == 0);
    return 0;
}
