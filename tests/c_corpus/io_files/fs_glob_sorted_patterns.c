/*
 * title: glob() patterns, flags and sorted results checked against a hand-rolled expander
 * topic: io_files
 * covers: glob, GLOB_MARK, GLOB_NOCHECK, GLOB_NOMATCH, GLOB_APPEND, GLOB_NOESCAPE, multi-segment patterns, leading dots, C-locale ordering, cross-check with fnmatch expansion
 * deps: libc, posix
 */
#include <fnmatch.h>
#include <glob.h>
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


/* independent expander: split on '/', match each segment against directory entries with FNM_PERIOD */
static char *res[256];
static int nres;

static void expand(const char *prefix, const char *pat) {
    while (*pat == '/') pat++;
    if (!*pat) {
        CHECK(nres < 256);
        res[nres++] = strdup(prefix);
        return;
    }
    char seg[64];
    size_t l = strcspn(pat, "/");
    CHECK(l < sizeof seg);
    memcpy(seg, pat, l);
    seg[l] = 0;
    const char *rest = pat + l;
    int n;
    char **v = ls_dir(prefix[0] ? prefix : ".", &n);
    if (!v) return;
    for (int i = 0; i < n; i++) {
        if (fnmatch(seg, v[i], FNM_PERIOD) != 0) continue;
        char *p = prefix[0] ? join2(prefix, v[i]) : strdup(v[i]);
        struct stat st;
        if (*rest && !(stat(p, &st) == 0 && S_ISDIR(st.st_mode))) {
            free(p);
            continue;
        }
        expand(p, rest);
        free(p);
    }
    ls_free(v, n);
}

static int cmp_res(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void check_pattern(const char *pat) {
    nres = 0;
    expand("", pat);
    if (pat[strlen(pat) - 1] == '/') /* glob keeps a trailing slash on directory matches */
        for (int i = 0; i < nres; i++) {
            size_t l = strlen(res[i]);
            res[i] = realloc(res[i], l + 2);
            res[i][l] = '/';
            res[i][l + 1] = 0;
        }
    qsort(res, (size_t)nres, sizeof res[0], cmp_res);
    glob_t g;
    int rc = glob(pat, 0, NULL, &g);
    if (nres == 0) {
        CHECK(rc == GLOB_NOMATCH);
        globfree(&g);
        printf("%-14s -> (no match)\n", pat);
        return;
    }
    CHECK(rc == 0 && (int)g.gl_pathc == nres);
    printf("%-14s ->", pat);
    for (int i = 0; i < nres; i++) {
        CHECK(strcmp(g.gl_pathv[i], res[i]) == 0);
        printf(" %s", g.gl_pathv[i]);
        free(res[i]);
    }
    printf("\n");
    globfree(&g);
}

int main(void) {
    umask(022);
    mkd("t");
    static const char *files[] = {
        "t/alpha.txt", "t/beta.txt", "t/Gamma.txt", "t/delta.c", "t/a1.c",  "t/a2.c",   "t/b10.c",
        "t/.hidden",   "t/.env.txt", "t/src/main.c", "t/src/util.c", "t/src/.gitkeep", "t/lib/liba.a",
        "t/lib/libb.so", "t/doc/readme", "t/doc/notes.txt", "t/a*b",
    };
    mkd("t/src");
    mkd("t/lib");
    mkd("t/doc");
    mkd("t/empty_dir");
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) put(files[i], "x");

    CHECK(chdir("t") == 0);
    static const char *pats[] = {
        "*.txt", "*.c", "?[0-9]*.c", "[a-b]*", "[!a-b]*", "*/*.c", "*/lib*", "*/", "s*/*", "*/.gitkeep",
        ".*.txt", "nothing*", "*.[ch]", "??????.txt", "d*/*", "*/*/*",
    };
    for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) check_pattern(pats[i]);

    /* flags */
    glob_t g;
    CHECK(glob("*.c", GLOB_MARK, NULL, &g) == 0);
    printf("GLOB_MARK on *.c: %zu paths, none end in '/': %d\n", g.gl_pathc, g.gl_pathv[0][strlen(g.gl_pathv[0]) - 1] != '/');
    globfree(&g);
    CHECK(glob("s*", GLOB_MARK, NULL, &g) == 0);
    printf("GLOB_MARK on s*: %s\n", g.gl_pathv[0]);
    CHECK(strcmp(g.gl_pathv[0], "src/") == 0 && g.gl_pathc == 1);
    globfree(&g);

    CHECK(glob("zzz*", GLOB_NOCHECK, NULL, &g) == 0);
    printf("GLOB_NOCHECK on zzz*: %zu path(s), first=%s\n", g.gl_pathc, g.gl_pathv[0]);
    CHECK(g.gl_pathc == 1 && strcmp(g.gl_pathv[0], "zzz*") == 0);
    globfree(&g);

    int rc = glob("zzz*", 0, NULL, &g);
    printf("no match without NOCHECK: %s\n", rc == GLOB_NOMATCH ? "GLOB_NOMATCH" : "other");
    CHECK(rc == GLOB_NOMATCH);
    globfree(&g);

    /* GLOB_APPEND concatenates result lists: each call is sorted, the whole is not re-sorted */
    CHECK(glob("*.txt", 0, NULL, &g) == 0);
    CHECK(glob("*.c", GLOB_APPEND, NULL, &g) == 0);
    printf("GLOB_APPEND: %zu paths:", g.gl_pathc);
    for (size_t i = 0; i < g.gl_pathc; i++) printf(" %s", g.gl_pathv[i]);
    printf("\n");
    CHECK(g.gl_pathc == 3 + 4);
    CHECK(strcmp(g.gl_pathv[0], "Gamma.txt") == 0 && strcmp(g.gl_pathv[3], "a1.c") == 0);
    globfree(&g);

    /* escapes: a file literally named a*b */
    CHECK(glob("a\\*b", 0, NULL, &g) == 0);
    printf("escaped star matches: %s (%zu)\n", g.gl_pathv[0], g.gl_pathc);
    CHECK(g.gl_pathc == 1);
    globfree(&g);
    rc = glob("a\\*b", GLOB_NOESCAPE, NULL, &g);
    printf("with GLOB_NOESCAPE the backslash is literal: %s\n", rc == GLOB_NOMATCH ? "GLOB_NOMATCH" : "matched");
    CHECK(rc == GLOB_NOMATCH);
    globfree(&g);
    CHECK(glob("a*b", 0, NULL, &g) == 0 && g.gl_pathc == 1);
    globfree(&g);

    CHECK(chdir("..") == 0);
    CHECK(rm_rf("t") == 0);
    return 0;
}
