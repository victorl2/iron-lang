/*
 * title: tree(1)-style directory printer with box drawing, limits and summary
 * topic: io_files
 * covers: recursive printer with prefix strings, last-entry connectors, hidden file filtering, depth limit, directories-only mode, symlink targets, size annotation, directory and file totals
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
    int show_hidden, dirs_only, max_depth, sizes, ascii;
    long dirs, files;
} Opt;

static void print_tree(const char *path, const char *prefix, int depth, Opt *o) {
    if (o->max_depth >= 0 && depth >= o->max_depth) return;
    int n;
    char **v = ls_dir(path, &n);
    CHECK(v);
    /* filter first so the "last entry" decision sees only visible entries */
    char **vis = malloc((size_t)(n ? n : 1) * sizeof *vis);
    struct stat *sts = malloc((size_t)(n ? n : 1) * sizeof *sts);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!o->show_hidden && v[i][0] == '.') continue;
        char *p = join2(path, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        free(p);
        if (o->dirs_only && !S_ISDIR(st.st_mode)) continue;
        vis[m] = v[i];
        sts[m] = st;
        m++;
    }
    for (int i = 0; i < m; i++) {
        int last = i == m - 1;
        const char *branch = o->ascii ? (last ? "`-- " : "|-- ") : (last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ");
        const char *cont = o->ascii ? (last ? "    " : "|   ") : (last ? "    " : "\xe2\x94\x82   ");
        printf("%s%s", prefix, branch);
        if (o->sizes && S_ISREG(sts[i].st_mode)) printf("[%6ld] ", (long)sts[i].st_size);
        else if (o->sizes) printf("[      ] ");
        printf("%s", vis[i]);
        char *p = join2(path, vis[i]);
        if (S_ISLNK(sts[i].st_mode)) {
            char t[128];
            ssize_t k = readlink(p, t, sizeof t - 1);
            CHECK(k >= 0);
            t[k] = 0;
            printf(" -> %s", t);
        }
        printf("\n");
        if (S_ISDIR(sts[i].st_mode)) {
            o->dirs++;
            char np[256];
            snprintf(np, sizeof np, "%s%s", prefix, cont);
            print_tree(p, np, depth + 1, o);
        } else {
            o->files++;
        }
        free(p);
    }
    free(vis);
    free(sts);
    ls_free(v, n);
}

static void run(const char *title, Opt o) {
    printf("$ tree %s\n", title);
    o.dirs = o.files = 0;
    printf(".\n");
    print_tree("root", "", 0, &o);
    if (o.dirs_only) printf("\n%ld directories\n\n", o.dirs);
    else printf("\n%ld directories, %ld files\n\n", o.dirs, o.files);
}

int main(void) {
    umask(022);
    mkd("root");
    static const char *dirs[] = {"root/src", "root/src/core", "root/src/core/impl", "root/docs", "root/empty",
                                  "root/.cache", "root/zeta"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) mkd(dirs[i]);
    static const struct {
        const char *path;
        int size;
    } files[] = {
        {"root/README.md", 1200}, {"root/Makefile", 310},      {"root/.gitignore", 12},  {"root/src/main.c", 4096},
        {"root/src/core/a.c", 77}, {"root/src/core/a.h", 5},   {"root/src/core/impl/x.c", 999999},
        {"root/docs/index.md", 88}, {"root/.cache/blob", 3},   {"root/zeta/last", 0},
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char *b = malloc((size_t)files[i].size + 1);
        memset(b, 'q', (size_t)files[i].size);
        b[files[i].size] = 0;
        put(files[i].path, b);
        free(b);
    }
    CHECK(symlink("src/main.c", "root/entry") == 0);
    CHECK(symlink("../docs", "root/src/docs_link") == 0);

    Opt o = {0, 0, -1, 0, 0, 0, 0};
    run("(default)", o);
    o.show_hidden = 1;
    run("-a", o);
    o.show_hidden = 0;
    o.dirs_only = 1;
    run("-d", o);
    o.dirs_only = 0;
    o.max_depth = 2;
    run("-L 2", o);
    o.max_depth = -1;
    o.sizes = 1;
    o.ascii = 1;
    run("-s --charset=ascii", o);
    CHECK(rm_rf("root") == 0);
    return 0;
}
