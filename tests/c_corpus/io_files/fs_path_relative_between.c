/*
 * title: Relative path from one path to another, verified by round trip
 * topic: io_files
 * covers: relative path computation, common prefix by components, ".." prefixes, normalize-apply round trip, random path pairs, chdir plus stat identity check on a real tree
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


#define MAXC 16

typedef struct {
    char part[MAXC][16];
    int n;
} Path;

static Path split(const char *s) {
    Path p;
    p.n = 0;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        int l = 0;
        while (s[l] && s[l] != '/') l++;
        CHECK(p.n < MAXC && l < 16);
        memcpy(p.part[p.n], s, (size_t)l);
        p.part[p.n][l] = 0;
        p.n++;
        s += l;
    }
    return p;
}

/* both inputs are normalized, rooted at the same origin */
static void relpath(const char *from, const char *to, char *out, size_t cap) {
    Path a = split(from), b = split(to);
    int c = 0;
    while (c < a.n && c < b.n && !strcmp(a.part[c], b.part[c])) c++;
    size_t o = 0;
    for (int i = c; i < a.n; i++) {
        if (o) out[o++] = '/';
        out[o++] = '.';
        out[o++] = '.';
    }
    for (int i = c; i < b.n; i++) {
        if (o) out[o++] = '/';
        size_t l = strlen(b.part[i]);
        CHECK(o + l + 4 < cap);
        memcpy(out + o, b.part[i], l);
        o += l;
    }
    if (!o) out[o++] = '.';
    out[o] = 0;
}

/* apply a relative path to a base and normalize the result */
static void apply(const char *base, const char *rel, char *out, size_t cap) {
    Path p = split(base), r = split(rel);
    for (int i = 0; i < r.n; i++) {
        if (!strcmp(r.part[i], "..")) {
            CHECK(p.n > 0);
            p.n--;
        } else if (strcmp(r.part[i], ".")) {
            CHECK(p.n < MAXC);
            snprintf(p.part[p.n++], 16, "%s", r.part[i]);
        }
    }
    size_t o = 0;
    for (int i = 0; i < p.n; i++) {
        if (i) out[o++] = '/';
        size_t l = strlen(p.part[i]);
        CHECK(o + l + 2 < cap);
        memcpy(out + o, p.part[i], l);
        o += l;
    }
    out[o] = 0;
}

static void rand_path(uint32_t *s, char *out, size_t cap) {
    static const char *alpha[] = {"a", "b", "c", "lib", "src"};
    uint32_t depth = rng_next(s) % 5;
    size_t o = 0;
    for (uint32_t i = 0; i < depth; i++) {
        uint32_t k = rng_next(s) % 5;
        if (o) out[o++] = '/';
        size_t l = strlen(alpha[k]);
        CHECK(o + l + 2 < cap);
        memcpy(out + o, alpha[k], l);
        o += l;
    }
    out[o] = 0;
}

int main(void) {
    umask(022);
    static const char *fixed[][2] = {
        {"a/b/c", "a/b/c/d/e"}, {"a/b/c/d/e", "a/b/c"}, {"a/b/c", "a/b/x"}, {"a/b", "c/d"},
        {"", "x/y"},           {"x/y", ""},            {"a", "a"},         {"a/b/c", "a/b"},
        {"a/b", "a/bc"},       {"lib/src", "lib/srcs"},
    };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        char rel[128], back[128];
        relpath(fixed[i][0], fixed[i][1], rel, sizeof rel);
        apply(fixed[i][0], rel, back, sizeof back);
        CHECK(strcmp(back, fixed[i][1]) == 0);
        printf("from [%s] to [%s] : %s\n", fixed[i][0], fixed[i][1], rel);
    }

    /* random pairs: apply(from, relpath(from, to)) == to, and rel(to, from) is its mirror in length */
    uint32_t seed = 0xACE1u;
    int longest = 0, ups_total = 0;
    for (int t = 0; t < 2000; t++) {
        char f[128], g[128], rel[128], rev[128], back[128];
        rand_path(&seed, f, sizeof f);
        rand_path(&seed, g, sizeof g);
        relpath(f, g, rel, sizeof rel);
        relpath(g, f, rev, sizeof rev);
        apply(f, rel, back, sizeof back);
        CHECK(strcmp(back, g) == 0);
        apply(g, rev, back, sizeof back);
        CHECK(strcmp(back, f) == 0);
        int ups = 0;
        for (const char *p = rel; (p = strstr(p, "..")) != NULL; p += 2) ups++;
        ups_total += ups;
        if ((int)strlen(rel) > longest) longest = (int)strlen(rel);
    }
    printf("2000 random pairs round-trip; total '..' steps=%d, longest relative path=%d\n", ups_total, longest);

    /* on a real tree: chdir into `from`, then the relative path must reach the same inode as `to` */
    mkd("t");
    mkd("t/a");
    mkd("t/a/b");
    mkd("t/a/b/c");
    mkd("t/lib");
    mkd("t/lib/src");
    put("t/lib/src/leaf", "L");
    put("t/a/b/c/leaf", "C");
    static const char *pairs[][2] = {
        {"t/a/b/c", "t/lib/src/leaf"}, {"t/lib", "t/a/b/c/leaf"}, {"t/a/b", "t/a"}, {"t/lib/src", "t/lib/src/leaf"},
    };
    int home = open(".", O_RDONLY | O_DIRECTORY);
    CHECK(home >= 0);
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        char rel[128];
        relpath(pairs[i][0], pairs[i][1], rel, sizeof rel);
        struct stat want, got;
        CHECK(stat(pairs[i][1], &want) == 0);
        CHECK(chdir(pairs[i][0]) == 0);
        CHECK(stat(rel, &got) == 0);
        CHECK(fchdir(home) == 0);
        CHECK(want.st_ino == got.st_ino && want.st_dev == got.st_dev);
        printf("real tree: %s -> %s via %s\n", pairs[i][0], pairs[i][1], rel);
    }
    close(home);
    CHECK(rm_rf("t") == 0);
    return 0;
}
