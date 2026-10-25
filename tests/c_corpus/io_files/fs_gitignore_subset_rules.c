/*
 * title: .gitignore-style rule engine built on fnmatch
 * topic: io_files
 * covers: fnmatch with FNM_PATHNAME, basename vs anchored patterns, directory-only rules, negation, last-match-wins, ignored directories not descended, comments and blank lines, rule parsing from text
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
    char pat[64];
    int negate, dir_only, anchored, has_slash;
} Rule;

static Rule rules[32];
static int nrules;

static void parse_rules(const char *text) {
    const char *p = text;
    while (*p) {
        char line[96];
        size_t l = strcspn(p, "\n");
        CHECK(l < sizeof line);
        memcpy(line, p, l);
        line[l] = 0;
        p += l + (p[l] ? 1 : 0);
        while (l && (line[l - 1] == ' ' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        Rule *r = &rules[nrules];
        memset(r, 0, sizeof *r);
        char *q = line;
        if (*q == '!') {
            r->negate = 1;
            q++;
        }
        size_t ql = strlen(q);
        if (ql && q[ql - 1] == '/') {
            r->dir_only = 1;
            q[--ql] = 0;
        }
        if (*q == '/') {
            r->anchored = 1;
            q++;
        }
        r->has_slash = strchr(q, '/') != NULL;
        snprintf(r->pat, sizeof r->pat, "%s", q);
        nrules++;
    }
}

/* returns 1 if ignored; the last matching rule wins */
static int ignored(const char *rel, int is_dir) {
    int result = 0;
    const char *base = strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel;
    for (int i = 0; i < nrules; i++) {
        const Rule *r = &rules[i];
        if (r->dir_only && !is_dir) continue;
        int m;
        if (r->anchored || r->has_slash) m = fnmatch(r->pat, rel, FNM_PATHNAME) == 0;
        else m = fnmatch(r->pat, base, 0) == 0;
        if (m) result = !r->negate;
    }
    return result;
}

static int kept, dropped;

static void walk(const char *dir, const char *rel) {
    int n;
    char **v = ls_dir(dir, &n);
    CHECK(v);
    for (int i = 0; i < n; i++) {
        char *p = join2(dir, v[i]);
        char relp[128];
        snprintf(relp, sizeof relp, "%s%s%s", rel, rel[0] ? "/" : "", v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        int is_dir = S_ISDIR(st.st_mode);
        if (ignored(relp, is_dir)) {
            printf("  ignore %s%s\n", relp, is_dir ? "/" : "");
            dropped++;
        } else if (is_dir) {
            walk(p, relp);
        } else {
            printf("  keep   %s\n", relp);
            kept++;
        }
        free(p);
    }
    ls_free(v, n);
}

static void build(void) {
    static const char *dirs[] = {"proj", "proj/build", "proj/src", "proj/src/build", "proj/docs", "proj/docs/api", "proj/logs", "proj/node_modules", "proj/node_modules/dep"};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) mkd(dirs[i]);
    static const char *files[] = {
        "proj/main.c",           "proj/main.o",          "proj/keep.o",           "proj/notes.tmp",
        "proj/build/out.bin",    "proj/build/keep.txt",  "proj/src/util.c",       "proj/src/util.o",
        "proj/src/build/x.c",    "proj/docs/guide.md",   "proj/docs/draft.tmp",   "proj/docs/api/ref.md",
        "proj/docs/api/wip.tmp", "proj/logs/a.log",      "proj/logs/b.log",       "proj/logs/keep.log",
        "proj/node_modules/dep/index.js", "proj/TODO",   "proj/src/TODO",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) put(files[i], "x");
}

int main(void) {
    umask(022);
    build();
    /* note: an ignored directory is never entered, so "!keep.txt" cannot rescue build/keep.txt */
    const char *text =
        "# compiled objects\n"
        "*.o\n"
        "!keep.o\n"
        "\n"
        "build/\n"
        "/TODO\n"
        "docs/*.tmp\n"
        "*.tmp\n"
        "!docs/api/*.tmp\n"
        "logs/*.log\n"
        "!logs/keep.log\n"
        "node_modules/   \n"
        "!keep.txt\n";
    parse_rules(text);
    printf("%d rules parsed\n", nrules);
    for (int i = 0; i < nrules; i++)
        printf("  rule %2d: %-14s neg=%d dir_only=%d anchored=%d slash=%d\n", i + 1, rules[i].pat, rules[i].negate,
               rules[i].dir_only, rules[i].anchored, rules[i].has_slash);
    CHECK(nrules == 11);

    printf("walk:\n");
    walk("proj", "");
    printf("kept=%d ignored=%d\n", kept, dropped);

    /* point checks of the decision function */
    struct {
        const char *path;
        int is_dir, want;
    } pc[] = {
        {"a/b/c.o", 0, 1}, {"keep.o", 0, 0}, {"x/keep.o", 0, 0}, {"build", 1, 1}, {"build", 0, 0},
        {"src/build", 1, 1}, {"TODO", 0, 1}, {"src/TODO", 0, 0}, {"docs/a.tmp", 0, 1}, {"docs/api/z.tmp", 0, 0},
        {"other/z.tmp", 0, 1}, {"logs/x.log", 0, 1}, {"logs/keep.log", 0, 0}, {"deep/logs/x.log", 0, 0},
    };
    for (size_t i = 0; i < sizeof pc / sizeof pc[0]; i++) {
        int got = ignored(pc[i].path, pc[i].is_dir);
        CHECK(got == pc[i].want);
    }
    printf("14 point checks of ignored() pass\n");
    CHECK(rm_rf("proj") == 0);
    return 0;
}
