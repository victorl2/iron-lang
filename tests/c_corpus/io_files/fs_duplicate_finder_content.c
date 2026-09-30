/*
 * title: Duplicate file finder by size, hash and byte comparison
 * topic: io_files
 * covers: three-stage duplicate detection, FNV-1a 64 hashing of file contents, byte-for-byte confirmation, hard links vs copies, empty files, symlinks skipped, wasted space report
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
    char path[64];
    long size;
    uint64_t hash;
    dev_t dev;
    ino_t ino;
    int group;
} Ent;

static Ent ents[64];
static int nents;

static uint64_t fnv64(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void collect(const char *dir) {
    int n;
    char **v = ls_dir(dir, &n);
    CHECK(v);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode)) collect(p);
        else if (S_ISREG(st.st_mode)) {
            CHECK(nents < 64);
            Ent *e = &ents[nents++];
            snprintf(e->path, sizeof e->path, "%s", p);
            e->size = (long)st.st_size;
            e->dev = st.st_dev;
            e->ino = st.st_ino;
            e->group = -1;
            size_t len;
            unsigned char *b = slurp(p, &len);
            CHECK(b && (long)len == e->size);
            e->hash = fnv64(b, len);
            free(b);
        } /* symlinks and everything else are ignored */
        free(p);
    }
    ls_free(v, n);
}

static int same_bytes(const Ent *a, const Ent *b) {
    size_t la, lb;
    unsigned char *x = slurp(a->path, &la), *y = slurp(b->path, &lb);
    int same = x && y && la == lb && memcmp(x, y, la) == 0;
    free(x);
    free(y);
    return same;
}

static int cmp_ent(const void *a, const void *b) { return strcmp(((const Ent *)a)->path, ((const Ent *)b)->path); }

int main(void) {
    umask(022);
    mkd("pool");
    mkd("pool/a");
    mkd("pool/b");
    mkd("pool/b/c");
    /* content pool: index -> bytes; several files draw the same content */
    static const char *pool[] = {"first content\n", "second content!\n", "", "same size AAAA", "same size BBBB", "z"};
    uint32_t seed = 31337;
    static const char *dirs[] = {"pool", "pool/a", "pool/b", "pool/b/c"};
    for (int i = 0; i < 16; i++) {
        uint32_t ci = rng_next(&seed) % 6;
        uint32_t di = rng_next(&seed) % 4;
        char p[64];
        snprintf(p, sizeof p, "%s/f%02d.dat", dirs[di], i);
        put(p, pool[ci]);
    }
    /* a hard link and a symlink to a file of known content */
    put("pool/keep.dat", "first content\n");
    CHECK(link("pool/keep.dat", "pool/b/hard_of_keep") == 0);
    CHECK(symlink("../keep.dat", "pool/a/sym_of_keep") == 0);
    /* two long files that share size and first bytes but differ at the very end */
    unsigned char big1[3000], big2[3000];
    for (int i = 0; i < 3000; i++) big1[i] = big2[i] = (unsigned char)(i * 7);
    big2[2999] ^= 1;
    CHECK(put_bytes("pool/a/big1", big1, 3000) == 0);
    CHECK(put_bytes("pool/b/c/big2", big2, 3000) == 0);
    CHECK(put_bytes("pool/b/big1_copy", big1, 3000) == 0);

    collect("pool");
    qsort(ents, (size_t)nents, sizeof ents[0], cmp_ent);
    printf("regular files scanned: %d\n", nents);

    /* stage 1+2: candidate = same size and same hash; stage 3: byte compare; hard links share an inode */
    int ngroups = 0;
    int group_first[64];
    for (int i = 0; i < nents; i++) {
        for (int g = 0; g < ngroups && ents[i].group < 0; g++) {
            Ent *rep = &ents[group_first[g]];
            if (rep->size != ents[i].size || rep->hash != ents[i].hash) continue;
            if (same_bytes(rep, &ents[i])) ents[i].group = g;
        }
        if (ents[i].group < 0) {
            group_first[ngroups] = i;
            ents[i].group = ngroups++;
        }
    }

    long wasted = 0;
    int dup_groups = 0;
    for (int g = 0; g < ngroups; g++) {
        int members = 0;
        long size = ents[group_first[g]].size;
        for (int i = 0; i < nents; i++)
            if (ents[i].group == g) members++;
        if (members < 2) continue;
        dup_groups++;
        printf("group size=%ld x%d\n", size, members);
        /* distinct inodes in the group = real copies; hard links do not waste space */
        int distinct = 0;
        for (int i = 0; i < nents; i++) {
            if (ents[i].group != g) continue;
            int first_inode = 1;
            for (int j = 0; j < i; j++)
                if (ents[j].group == g && ents[j].ino == ents[i].ino && ents[j].dev == ents[i].dev) first_inode = 0;
            distinct += first_inode;
            printf("  %s%s\n", ents[i].path, first_inode ? "" : "  (hard link)");
        }
        wasted += (long)(distinct - 1) * size;
    }
    printf("duplicate groups: %d, wasted bytes: %ld\n", dup_groups, wasted);

    /* the near-miss pair must not be grouped */
    int g1 = -1, g2 = -1;
    for (int i = 0; i < nents; i++) {
        if (!strcmp(ents[i].path, "pool/a/big1")) g1 = ents[i].group;
        if (!strcmp(ents[i].path, "pool/b/c/big2")) g2 = ents[i].group;
    }
    CHECK(g1 >= 0 && g2 >= 0 && g1 != g2);
    printf("near-miss big1/big2 kept apart: yes\n");

    /* brute force cross-check: pairwise equality must match group membership */
    long pairs = 0;
    for (int i = 0; i < nents; i++)
        for (int j = i + 1; j < nents; j++) {
            int eq = ents[i].size == ents[j].size && same_bytes(&ents[i], &ents[j]);
            CHECK(eq == (ents[i].group == ents[j].group));
            pairs += eq;
        }
    printf("equal pairs by brute force: %ld\n", pairs);
    CHECK(rm_rf("pool") == 0);
    return 0;
}
