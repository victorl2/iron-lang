/*
 * title: Randomized create/unlink/rename churn checked against an in-memory model
 * topic: io_files
 * covers: open O_EXCL, mkdir, unlink, rmdir, rename of files and directories, success/failure agreement with a model, periodic full-tree comparison, content tracking through renames
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


#define MAXN 1024

typedef struct {
    char path[96];
    int is_dir;
    int content; /* files only */
    int live;
} Node;

static Node model[MAXN];

static Node *find(const char *p) {
    for (int i = 0; i < MAXN; i++)
        if (model[i].live && !strcmp(model[i].path, p)) return &model[i];
    return NULL;
}

static Node *add(const char *p, int is_dir, int content) {
    for (int i = 0; i < MAXN; i++)
        if (!model[i].live) {
            model[i].live = 1;
            snprintf(model[i].path, sizeof model[i].path, "%s", p);
            model[i].is_dir = is_dir;
            model[i].content = content;
            return &model[i];
        }
    return NULL;
}

static void parent_of(const char *p, char *out) {
    const char *s = strrchr(p, '/');
    if (!s) out[0] = 0;
    else {
        memcpy(out, p, (size_t)(s - p));
        out[s - p] = 0;
    }
}

/* parent state: 0 = fine, 1 = missing, 2 = not a directory */
static int parent_state(const char *p) {
    char par[96];
    parent_of(p, par);
    if (!par[0]) return 0;
    Node *n = find(par);
    if (!n) return 1;
    return n->is_dir ? 0 : 2;
}

static int has_children(const char *p) {
    size_t l = strlen(p);
    for (int i = 0; i < MAXN; i++)
        if (model[i].live && !strncmp(model[i].path, p, l) && model[i].path[l] == '/') return 1;
    return 0;
}

static void rand_path(uint32_t *s, char *out) {
    static const char *nm[] = {"a", "b", "c", "d"};
    uint32_t depth = 1 + rng_next(s) % 3;
    out[0] = 0;
    for (uint32_t i = 0; i < depth; i++) {
        if (i) strcat(out, "/");
        strcat(out, nm[rng_next(s) % 4]);
    }
}

/* longest path any node would get if `from` moved to `to` */
static size_t moved_len(const char *from, const char *to) {
    size_t lf = strlen(from), best = 0;
    for (int i = 0; i < MAXN; i++) {
        if (!model[i].live) continue;
        if (!strcmp(model[i].path, from) || (!strncmp(model[i].path, from, lf) && model[i].path[lf] == '/')) {
            size_t l = strlen(to) + strlen(model[i].path) - lf;
            if (l > best) best = l;
        }
    }
    return best;
}

static void rename_model(const char *from, const char *to) {
    size_t lf = strlen(from);
    Node moved[MAXN];
    int nm = 0;
    for (int i = 0; i < MAXN; i++) {
        if (!model[i].live) continue;
        if (!strcmp(model[i].path, from) || (!strncmp(model[i].path, from, lf) && model[i].path[lf] == '/')) {
            moved[nm] = model[i];
            model[i].live = 0;
            nm++;
        }
    }
    Node *victim = find(to);
    if (victim) victim->live = 0;
    for (int i = 0; i < nm; i++) {
        char np[96];
        snprintf(np, sizeof np, "%s%s", to, moved[i].path + lf);
        Node *n = add(np, moved[i].is_dir, moved[i].content);
        CHECK(n != NULL);
    }
}

/* full comparison: every model node exists with the right type and content, and nothing else does */
static int cmp_paths(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void collect(const char *dir, const char *rel, char (*out)[96], int *n) {
    int cnt;
    char **v = ls_dir(dir, &cnt);
    CHECK(v);
    for (int i = 0; i < cnt; i++) {
        char *p = join2(dir, v[i]);
        char relp[96];
        snprintf(relp, sizeof relp, "%s%s%s", rel, rel[0] ? "/" : "", v[i]);
        CHECK(*n < MAXN);
        snprintf(out[(*n)++], 96, "%s", relp);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        if (S_ISDIR(st.st_mode)) collect(p, relp, out, n);
        free(p);
    }
    ls_free(v, cnt);
}

static void verify(void) {
    static char fs[MAXN][96];
    int nfs = 0;
    collect("w", "", fs, &nfs);
    qsort(fs, (size_t)nfs, 96, cmp_paths);
    static char md[MAXN][96];
    int nmd = 0;
    for (int i = 0; i < MAXN; i++)
        if (model[i].live) snprintf(md[nmd++], 96, "%s", model[i].path);
    qsort(md, (size_t)nmd, 96, cmp_paths);
    CHECK(nfs == nmd);
    for (int i = 0; i < nfs; i++) CHECK(strcmp(fs[i], md[i]) == 0);
    for (int i = 0; i < MAXN; i++) {
        if (!model[i].live) continue;
        char p[128];
        snprintf(p, sizeof p, "w/%s", model[i].path);
        struct stat st;
        CHECK(lstat(p, &st) == 0 && (S_ISDIR(st.st_mode) != 0) == model[i].is_dir);
        if (!model[i].is_dir) {
            size_t len;
            unsigned char *b = slurp(p, &len);
            char exp[16];
            snprintf(exp, sizeof exp, "c%d", model[i].content);
            CHECK(b && !strcmp((char *)b, exp));
            free(b);
        }
    }
}

int main(void) {
    umask(022);
    mkd("w");
    uint32_t seed = 20240607;
    long ok[5] = {0}, fail[5] = {0};
    static const char *opname[5] = {"create", "mkdir", "unlink", "rmdir", "rename"};
    int next_content = 1;
    const int OPS = 4000;
    for (int step = 0; step < OPS; step++) {
        uint32_t op = rng_next(&seed) % 6;
        if (op == 5) op = 4; /* renames are the interesting ones: double their weight */
        char p[32], q[32], fp[64], fq[64];
        rand_path(&seed, p);
        rand_path(&seed, q);
        snprintf(fp, sizeof fp, "w/%s", p);
        snprintf(fq, sizeof fq, "w/%s", q);
        int expect_ok, actual_ok;
        Node *np = find(p);
        int ps = parent_state(p);
        switch (op) {
        case 0: { /* create file with O_EXCL */
            expect_ok = ps == 0 && !np && add(p, 0, 0) != NULL;
            if (expect_ok) find(p)->content = next_content;
            int fd = open(fp, O_WRONLY | O_CREAT | O_EXCL, 0644);
            actual_ok = fd >= 0;
            if (fd >= 0) {
                char buf[16];
                snprintf(buf, sizeof buf, "c%d", next_content);
                CHECK(write(fd, buf, strlen(buf)) == (ssize_t)strlen(buf));
                close(fd);
                next_content++;
            }
            break;
        }
        case 1:
            expect_ok = ps == 0 && !np && add(p, 1, 0) != NULL;
            actual_ok = mkdir(fp, 0755) == 0;
            break;
        case 2:
            expect_ok = np && !np->is_dir;
            if (expect_ok) np->live = 0;
            actual_ok = unlink(fp) == 0;
            break;
        case 3:
            expect_ok = np && np->is_dir && !has_children(p);
            if (expect_ok) np->live = 0;
            actual_ok = rmdir(fp) == 0;
            break;
        default: { /* rename */
            Node *nq = find(q);
            int qs = parent_state(q);
            if (np && moved_len(p, q) > 60) continue; /* keep paths short: skip this op on both sides */
            size_t lp = strlen(p);
            int into_self = !strncmp(q, p, lp) && q[lp] == '/';
            if (!np || qs != 0) expect_ok = 0;
            else if (!strcmp(p, q)) expect_ok = 1;
            else if (into_self) expect_ok = 0;
            else if (!nq) expect_ok = 1;
            else if (np->is_dir != nq->is_dir) expect_ok = 0;
            else if (np->is_dir && has_children(q)) expect_ok = 0;
            else expect_ok = 1;
            actual_ok = rename(fp, fq) == 0;
            if (expect_ok && actual_ok && strcmp(p, q)) rename_model(p, q);
            break;
        }
        }
        if (expect_ok != actual_ok) {
            fprintf(stderr, "step %d op %s p=%s q=%s expected %d got %d\n", step, opname[op], p, q, expect_ok, actual_ok);
            exit(1);
        }
        if (actual_ok) ok[op]++;
        else fail[op]++;
        if (step % 250 == 249) verify();
    }
    verify();
    for (int i = 0; i < 5; i++) printf("%-7s ok=%ld refused=%ld\n", opname[i], ok[i], fail[i]);
    int live = 0, dirs = 0;
    for (int i = 0; i < MAXN; i++) {
        live += model[i].live;
        dirs += model[i].live && model[i].is_dir;
    }
    printf("final tree: %d nodes, %d directories\n", live, dirs);
    CHECK(rm_rf("w") == 0);
    return 0;
}
