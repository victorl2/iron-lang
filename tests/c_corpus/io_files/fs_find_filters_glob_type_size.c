/*
 * title: find-like tree search with name, type, size and depth predicates
 * topic: io_files
 * covers: fnmatch on basenames, type filters, size comparisons, mindepth/maxdepth, prune by name, empty files, two-phase filter cross-check
 * deps: libc, posix
 */
#include <fnmatch.h>
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
    const char *name_glob; /* NULL = any */
    char type;             /* 0 = any, else one of f d l */
    long size_min, size_max; /* -1 = unbounded; applies to regular files */
    int mindepth, maxdepth;  /* maxdepth < 0 = unbounded */
    const char *prune;     /* directory basename glob not to descend into */
} Query;

static int matches(const Query *q, const char *path, const char *base, int depth, const struct stat *st) {
    if (depth < q->mindepth) return 0;
    if (q->maxdepth >= 0 && depth > q->maxdepth) return 0;
    char k = S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : 'f';
    if (q->type && q->type != k) return 0;
    if (q->name_glob && fnmatch(q->name_glob, base, 0) != 0) return 0;
    if (q->size_min >= 0 || q->size_max >= 0) {
        if (k != 'f') return 0;
        if (q->size_min >= 0 && st->st_size < q->size_min) return 0;
        if (q->size_max >= 0 && st->st_size > q->size_max) return 0;
    }
    (void)path;
    return 1;
}

static char *out[256];
static int nout;

static void search(const Query *q, const char *dir, int depth, int use_prune) {
    int n;
    char **v = ls_dir(dir, &n);
    CHECK(v);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        if (matches(q, p, v[i], depth + 1, &st)) {
            CHECK(nout < 256);
            out[nout++] = strdup(p);
        }
        int descend = S_ISDIR(st.st_mode) && (q->maxdepth < 0 || depth + 1 < q->maxdepth + 1);
        if (descend && use_prune && q->prune && fnmatch(q->prune, v[i], 0) == 0) descend = 0;
        if (descend) search(q, p, depth + 1, use_prune);
        free(p);
    }
    ls_free(v, n);
}

static void clear_out(void) {
    for (int i = 0; i < nout; i++) free(out[i]);
    nout = 0;
}

static void run(const char *label, const Query *q) {
    clear_out();
    search(q, "w", 0, 1);
    printf("find %s -> %d\n", label, nout);
    for (int i = 0; i < nout; i++) printf("  %s\n", out[i]);
    /* cross-check: without a prune, the walk with pruning disabled must find the same count */
    if (!q->prune) {
        int first = nout;
        clear_out();
        search(q, "w", 0, 0);
        CHECK(nout == first);
    }
}

int main(void) {
    umask(022);
    mkd("w");
    mkd("w/src");
    mkd("w/src/core");
    mkd("w/build");
    mkd("w/build/obj");
    mkd("w/docs");
    mkd("w/.git");
    static const struct {
        const char *path;
        int size;
    } files[] = {
        {"w/Makefile", 120},          {"w/README.md", 800},         {"w/src/main.c", 2100},
        {"w/src/util.c", 640},        {"w/src/util.h", 90},         {"w/src/core/alloc.c", 3300},
        {"w/src/core/alloc.h", 0},    {"w/build/obj/main.o", 5000}, {"w/build/obj/util.o", 1200},
        {"w/build/app", 9000},        {"w/docs/guide.md", 1500},    {"w/docs/empty.md", 0},
        {"w/.git/HEAD", 23},          {"w/.git/config", 210},       {"w/.hidden", 5},
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        char *buf = malloc((size_t)files[i].size + 1);
        memset(buf, 'x', (size_t)files[i].size);
        buf[files[i].size] = 0;
        put(files[i].path, buf);
        free(buf);
    }
    CHECK(symlink("src/main.c", "w/main_link.c") == 0);
    CHECK(symlink("docs", "w/docs_link") == 0);

    Query all = {NULL, 0, -1, -1, 0, -1, NULL};
    Query q;

    q = all;
    q.name_glob = "*.c";
    q.type = 'f';
    run("-name '*.c' -type f", &q);

    q = all;
    q.name_glob = "*.[ch]";
    q.type = 0;
    run("-name '*.[ch]'", &q);

    q = all;
    q.type = 'd';
    run("-type d", &q);

    q = all;
    q.type = 'l';
    run("-type l", &q);

    q = all;
    q.type = 'f';
    q.size_max = 0;
    run("-type f -empty", &q);

    q = all;
    q.type = 'f';
    q.size_min = 2000;
    run("-type f -size +1999c", &q);

    q = all;
    q.type = 'f';
    q.size_min = 100;
    q.size_max = 1000;
    run("-type f -size 100..1000c", &q);

    q = all;
    q.maxdepth = 1;
    run("-maxdepth 1", &q);

    q = all;
    q.mindepth = 3;
    run("-mindepth 3", &q);

    q = all;
    q.name_glob = "?????.?";
    q.type = 'f';
    run("-name '?????.?' -type f", &q);

    q = all;
    q.name_glob = "*.[co]";
    q.type = 'f';
    q.prune = "build";
    run("-name '*.[co]' -prune build", &q);

    q = all;
    q.type = 'f';
    q.prune = ".*";
    q.mindepth = 1;
    run("-type f -prune '.*'", &q);

    /* dotfiles are not matched by "*" unless FNM_PERIOD is off (we use flags 0) */
    q = all;
    q.name_glob = "*";
    q.type = 'f';
    q.maxdepth = 1;
    run("-name '*' -type f -maxdepth 1", &q);

    clear_out();
    CHECK(rm_rf("w") == 0);
    return 0;
}
