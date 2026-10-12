/*
 * title: Lexical path normalization vs kernel resolution
 * topic: io_files
 * covers: collapsing ., .., and //, absolute vs relative results, leading .. on relative paths, POSIX dirname/basename, symlink/.. divergence between lexical and kernel views
 * deps: libc, posix
 */
#include <libgen.h>
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


/* normalize into out: resolves "." and ".." lexically, squeezes slashes, keeps leading ".." for relative paths */
static void normalize(const char *in, char *out, size_t cap) {
    int abs_ = in[0] == '/';
    const char *comp[64];
    size_t len[64];
    int n = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t l = (size_t)(p - s);
        if (l == 1 && s[0] == '.') continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (n > 0 && !(len[n - 1] == 2 && comp[n - 1][0] == '.' && comp[n - 1][1] == '.')) n--;
            else if (!abs_) {
                CHECK(n < 64);
                comp[n] = s;
                len[n++] = l;
            } /* ".." at an absolute root stays at the root */
            continue;
        }
        CHECK(n < 64);
        comp[n] = s;
        len[n++] = l;
    }
    size_t o = 0;
    if (abs_) out[o++] = '/';
    for (int i = 0; i < n; i++) {
        if (i) out[o++] = '/';
        CHECK(o + len[i] + 2 < cap);
        memcpy(out + o, comp[i], len[i]);
        o += len[i];
    }
    if (o == 0) out[o++] = '.';
    out[o] = 0;
}

static const struct {
    const char *in, *want;
} cases[] = {
    {"a/b/c", "a/b/c"},
    {"a/./b//c/", "a/b/c"},
    {"a/b/../c", "a/c"},
    {"a/b/../../c", "c"},
    {"a/..", "."},
    {"", "."},
    {".", "."},
    {"./././", "."},
    {"../a", "../a"},
    {"../../a/../b", "../../b"},
    {"a/../../b", "../b"},
    {"/", "/"},
    {"/..", "/"},
    {"/../../x", "/x"},
    {"//x///y//", "/x/y"},
    {"/a/b/./../c/.", "/a/c"},
    {"a//b/.../c", "a/b/.../c"},   /* "..." is an ordinary name */
    {"..a/.b/b..", "..a/.b/b.."},
};

int main(void) {
    umask(022);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char out[128];
        normalize(cases[i].in, out, sizeof out);
        CHECK(strcmp(out, cases[i].want) == 0);
        char again[128];
        normalize(out, again, sizeof again);
        CHECK(strcmp(again, out) == 0); /* idempotent */
        printf("%-16s => %s\n", cases[i].in[0] ? cases[i].in : "(empty)", out);
    }

    /* the kernel agrees with lexical normalization while there are no symlinks */
    mkd("r");
    mkd("r/a");
    mkd("r/a/b");
    put("r/a/b/f", "x");
    put("r/top", "y");
    static const char *real[] = {"r/a/./b//f", "r/a/b/../b/f", "r/a/b/../../top", "./r//a/../top"};
    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++) {
        char nrm[64];
        normalize(real[i], nrm, sizeof nrm);
        struct stat s1, s2;
        CHECK(stat(real[i], &s1) == 0 && stat(nrm, &s2) == 0 && s1.st_ino == s2.st_ino);
        printf("same file: %-16s ~ %s\n", real[i], nrm);
    }

    /* with a symlink the kernel resolves ".." physically, lexical normalization does not */
    mkd("r/real");
    mkd("r/real/inner");
    put("r/real/marker", "m");
    CHECK(symlink("real/inner", "r/ln") == 0);
    char nrm[64];
    normalize("r/ln/../marker", nrm, sizeof nrm);
    struct stat st;
    int kernel = stat("r/ln/../marker", &st) == 0;
    int lexical = stat(nrm, &st) == 0;
    printf("r/ln/../marker: kernel finds it=%d, lexical form %s finds it=%d\n", kernel, nrm, lexical);
    CHECK(kernel == 1 && lexical == 0);

    /* POSIX dirname/basename on writable copies */
    static const char *paths[] = {"a/b/c", "a/b/", "/a", "/", "file", "a//b", "./x"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        char d[32], b[32];
        snprintf(d, sizeof d, "%s", paths[i]);
        snprintf(b, sizeof b, "%s", paths[i]);
        char *dn = dirname(d);
        char *bn = basename(b);
        printf("dirname/basename(%s) = [%s] [%s]\n", paths[i], dn, bn);
    }
    CHECK(rm_rf("r") == 0);
    return 0;
}
