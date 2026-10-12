/*
 * title: Manual recursive and iterative directory walks
 * topic: io_files
 * covers: opendir recursion, explicit stack DFS vs recursive DFS vs BFS queue, depth statistics, random tree generation, cross-check of traversal orders
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
    long files, dirs, bytes;
    int maxdepth;
    long per_depth[8];
    char deepest[128];
} Stats;

static uint32_t seed = 0xC0FFEEu;

/* random tree: each directory gets 1..4 files and, above depth 4, 0..3 subdirs */
static void gen(const char *path, int depth, int *counter) {
    uint32_t r = rng_next(&seed);
    int nf = 1 + (int)(r % 4);
    for (int i = 0; i < nf; i++) {
        char p[128];
        uint32_t sz = rng_next(&seed) % 97;
        char *buf = malloc(sz + 1);
        memset(buf, 'a' + (*counter % 26), sz);
        buf[sz] = 0;
        snprintf(p, sizeof p, "%s/f%02d_%d.dat", path, (*counter)++, i);
        put(p, buf);
        free(buf);
    }
    if (depth >= 4) return;
    uint32_t r2 = rng_next(&seed);
    int nd = (int)(r2 % 4);
    for (int i = 0; i < nd; i++) {
        char p[128];
        snprintf(p, sizeof p, "%s/d%d_%d", path, depth, i);
        mkd(p);
        gen(p, depth + 1, counter);
    }
}

static void note(Stats *s, const char *path, int depth, const struct stat *st) {
    if (S_ISDIR(st->st_mode)) s->dirs++;
    else {
        s->files++;
        s->bytes += (long)st->st_size;
    }
    if (depth > 7) depth = 7;
    s->per_depth[depth]++;
    if (depth > s->maxdepth || (depth == s->maxdepth && strcmp(path, s->deepest) < 0)) {
        s->maxdepth = depth;
        snprintf(s->deepest, sizeof s->deepest, "%s", path);
    }
}

static void walk_rec(const char *path, int depth, Stats *s) {
    int n;
    char **v = ls_dir(path, &n);
    CHECK(v);
    for (int i = 0; i < n; i++) {
        char *p = join2(path, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        note(s, p, depth + 1, &st);
        if (S_ISDIR(st.st_mode)) walk_rec(p, depth + 1, s);
        free(p);
    }
    ls_free(v, n);
}

/* explicit stack; entries pushed in reverse so pop order matches sorted recursion */
static void walk_stack(const char *root, Stats *s) {
    typedef struct {
        char *path;
        int depth;
    } Item;
    Item *st_ = malloc(4096 * sizeof *st_);
    int sp = 0;
    st_[sp].path = strdup(root);
    st_[sp++].depth = 0;
    while (sp > 0) {
        Item it = st_[--sp];
        int n;
        char **v = ls_dir(it.path, &n);
        CHECK(v);
        for (int i = n - 1; i >= 0; i--) {
            char *p = join2(it.path, v[i]);
            struct stat sb;
            CHECK(lstat(p, &sb) == 0);
            /* account when pushed; order differs from recursion, totals must not */
            note(s, p, it.depth + 1, &sb);
            if (S_ISDIR(sb.st_mode)) {
                CHECK(sp < 4096);
                st_[sp].path = p;
                st_[sp++].depth = it.depth + 1;
            } else free(p);
        }
        ls_free(v, n);
        free(it.path);
    }
    free(st_);
}

static void walk_bfs(const char *root, Stats *s, char *order, size_t ordcap) {
    char **q = malloc(4096 * sizeof *q);
    int *dq = malloc(4096 * sizeof *dq);
    int head = 0, tail = 0;
    q[tail] = strdup(root);
    dq[tail++] = 0;
    size_t ol = 0;
    while (head < tail) {
        char *path = q[head];
        int depth = dq[head++];
        int n;
        char **v = ls_dir(path, &n);
        CHECK(v);
        for (int i = 0; i < n; i++) {
            char *p = join2(path, v[i]);
            struct stat sb;
            CHECK(lstat(p, &sb) == 0);
            note(s, p, depth + 1, &sb);
            if (S_ISDIR(sb.st_mode)) {
                CHECK(tail < 4096);
                q[tail] = p;
                dq[tail++] = depth + 1;
                if (ol + 2 < ordcap) order[ol++] = 'D';
            } else {
                if (ol + 2 < ordcap) order[ol++] = 'f';
                free(p);
            }
        }
        ls_free(v, n);
        free(path);
    }
    order[ol] = 0;
    free(q);
    free(dq);
}

static void show(const char *tag, const Stats *s) {
    printf("%-5s files=%ld dirs=%ld bytes=%ld maxdepth=%d per_depth:", tag, s->files, s->dirs,
           s->bytes, s->maxdepth);
    for (int i = 1; i <= s->maxdepth; i++) printf(" %ld", s->per_depth[i]);
    printf("\n");
}

int main(void) {
    umask(022);
    mkd("root");
    int counter = 0;
    gen("root", 0, &counter);

    Stats a, b, c;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    memset(&c, 0, sizeof c);
    char order[512];
    walk_rec("root", 0, &a);
    walk_stack("root", &b);
    walk_bfs("root", &c, order, sizeof order);
    show("rec", &a);
    show("stack", &b);
    show("bfs", &c);
    printf("deepest: %s\n", a.deepest);
    CHECK(a.files == b.files && a.files == c.files);
    CHECK(a.dirs == b.dirs && a.dirs == c.dirs);
    CHECK(a.bytes == b.bytes && a.bytes == c.bytes);
    CHECK(a.maxdepth == b.maxdepth && a.maxdepth == c.maxdepth);
    for (int i = 0; i < 8; i++) CHECK(a.per_depth[i] == b.per_depth[i] && a.per_depth[i] == c.per_depth[i]);
    CHECK(strcmp(a.deepest, c.deepest) == 0);
    CHECK((long)strlen(order) == a.files + a.dirs);
    printf("bfs kind order: %s\n", order);

    /* removing the tree bottom-up by the same walk empties it */
    long removed = 0;
    CHECK(rm_at(AT_FDCWD, "root", &removed) == 0);
    CHECK(removed == a.files + a.dirs + 1);
    printf("removed %ld entries incl. root\n", removed);
    return 0;
}
