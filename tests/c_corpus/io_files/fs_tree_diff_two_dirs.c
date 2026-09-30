/*
 * title: Directory tree diff with merge-walk of sorted listings
 * topic: io_files
 * covers: sorted merge of two directory listings, added/removed/modified/type-changed/mode-changed classification, byte comparison, symlink target comparison, recursion into common directories
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
    int added, removed, content, size, type, mode, link;
} Tally;

static const char *kname(mode_t m) {
    if (S_ISREG(m)) return "file";
    if (S_ISDIR(m)) return "dir";
    if (S_ISLNK(m)) return "symlink";
    return "other";
}

static int same_bytes(const char *a, const char *b) {
    size_t la, lb;
    unsigned char *x = slurp(a, &la), *y = slurp(b, &lb);
    int same = x && y && la == lb && memcmp(x, y, la) == 0;
    free(x);
    free(y);
    return same;
}

static void diff_dirs(const char *a, const char *b, const char *rel, Tally *t) {
    int na, nb;
    char **va = ls_dir(a, &na), **vb = ls_dir(b, &nb);
    CHECK(va && vb);
    int i = 0, j = 0;
    while (i < na || j < nb) {
        int c = i >= na ? 1 : j >= nb ? -1 : strcmp(va[i], vb[j]);
        const char *name = c <= 0 ? va[i] : vb[j];
        char relp[128];
        snprintf(relp, sizeof relp, "%s%s%s", rel, rel[0] ? "/" : "", name);
        if (c < 0) {
            printf("- %s\n", relp);
            t->removed++;
            i++;
            continue;
        }
        if (c > 0) {
            printf("+ %s\n", relp);
            t->added++;
            j++;
            continue;
        }
        char *pa = join2(a, va[i]), *pb = join2(b, vb[j]);
        struct stat sa, sb;
        CHECK(lstat(pa, &sa) == 0 && lstat(pb, &sb) == 0);
        if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
            printf("T %s (%s -> %s)\n", relp, kname(sa.st_mode), kname(sb.st_mode));
            t->type++;
        } else if (S_ISLNK(sa.st_mode)) {
            char x[64], y[64];
            ssize_t nx = readlink(pa, x, sizeof x - 1), ny = readlink(pb, y, sizeof y - 1);
            CHECK(nx >= 0 && ny >= 0);
            x[nx] = y[ny] = 0;
            if (strcmp(x, y)) {
                printf("L %s (%s -> %s)\n", relp, x, y);
                t->link++;
            }
        } else {
            if ((sa.st_mode & 07777) != (sb.st_mode & 07777)) {
                printf("P %s (%04o -> %04o)\n", relp, (unsigned)(sa.st_mode & 07777),
                       (unsigned)(sb.st_mode & 07777));
                t->mode++;
            }
            if (S_ISREG(sa.st_mode)) {
                if (sa.st_size != sb.st_size) {
                    printf("S %s (%ld -> %ld bytes)\n", relp, (long)sa.st_size, (long)sb.st_size);
                    t->size++;
                } else if (!same_bytes(pa, pb)) {
                    printf("M %s (same size, different bytes)\n", relp);
                    t->content++;
                }
            } else if (S_ISDIR(sa.st_mode)) {
                diff_dirs(pa, pb, relp, t);
            }
        }
        free(pa);
        free(pb);
        i++;
        j++;
    }
    ls_free(va, na);
    ls_free(vb, nb);
}

static void write_tree(const char *root) {
    char p[96];
    mkd(root);
    static const char *dirs[] = {"docs", "src", "src/util", "assets", "tmp"};
    for (size_t i = 0; i < 5; i++) {
        snprintf(p, sizeof p, "%s/%s", root, dirs[i]);
        mkd(p);
    }
    static const char *files[][2] = {
        {"README", "readme v1\n"}, {"docs/guide.txt", "the guide\n"},  {"docs/faq.txt", "questions\n"},
        {"src/main.c", "int main(){}\n"}, {"src/util/a.c", "aaaa"}, {"src/util/b.c", "bbbb"},
        {"assets/logo.bin", "\x01\x02\x03\x04"}, {"tmp/scratch", "s"},
    };
    for (size_t i = 0; i < 8; i++) {
        snprintf(p, sizeof p, "%s/%s", root, files[i][0]);
        put(p, files[i][1]);
    }
    snprintf(p, sizeof p, "%s/latest", root);
    CHECK(symlink("docs/guide.txt", p) == 0);
}

int main(void) {
    umask(022);
    write_tree("A");
    write_tree("B");
    /* apply a known set of edits to B */
    put("B/README", "readme v2 with more text\n");     /* S: size change */
    put("B/src/util/a.c", "aaab");                      /* M: same size */
    CHECK(chmod("B/src/main.c", 0755) == 0);            /* P */
    CHECK(unlink("B/docs/faq.txt") == 0);               /* - */
    put("B/docs/new.txt", "fresh\n");                   /* + */
    mkd("B/src/extra");                                 /* + dir */
    CHECK(rm_rf("B/tmp") == 0);                         /* - dir */
    CHECK(unlink("B/assets/logo.bin") == 0);
    mkd("B/assets/logo.bin");                           /* T: file -> dir */
    CHECK(unlink("B/latest") == 0);
    CHECK(symlink("docs/new.txt", "B/latest") == 0);    /* L */
    CHECK(unlink("B/src/util/b.c") == 0);
    CHECK(symlink("a.c", "B/src/util/b.c") == 0);       /* T: file -> symlink */

    Tally t;
    memset(&t, 0, sizeof t);
    diff_dirs("A", "B", "", &t);
    printf("summary: +%d -%d content=%d size=%d type=%d mode=%d link=%d\n", t.added, t.removed,
           t.content, t.size, t.type, t.mode, t.link);
    CHECK(t.added == 2 && t.removed == 2 && t.content == 1 && t.size == 1 && t.type == 2 &&
          t.mode == 1 && t.link == 1);

    Tally z;
    memset(&z, 0, sizeof z);
    printf("--- self diff\n");
    diff_dirs("A", "A", "", &z);
    CHECK(z.added + z.removed + z.content + z.size + z.type + z.mode + z.link == 0);
    printf("A vs A: no differences\n");
    CHECK(rm_rf("A") == 0 && rm_rf("B") == 0);
    return 0;
}
