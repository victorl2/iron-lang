/*
 * title: Moving files and directory subtrees across directories
 * topic: io_files
 * covers: rename between directories, inode preserved on move, subtree move without copying, open fd survives rename, dotdot fix-up, renameat
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

static inline char kind_of(mode_t m) {
    if (S_ISREG(m)) return '-';
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    if (S_ISCHR(m)) return 'c';
    if (S_ISBLK(m)) return 'b';
    return '?';
}

static inline long fsize(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    return (long)st.st_size;
}

static inline int exists_nofollow(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

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

/* recursive sorted dump: kind, perms (not for symlinks), size (regular files), path */
static inline void dump_tree(const char *path) {
    int n;
    char **v = ls_dir(path, &n);
    CHECK(v != NULL);
    for (int i = 0; i < n; i++) {
        char *p = join2(path, v[i]);
        struct stat st;
        CHECK(lstat(p, &st) == 0);
        char k = kind_of(st.st_mode);
        if (k == 'l') {
            char tgt[128];
            ssize_t m = readlink(p, tgt, sizeof tgt - 1);
            CHECK(m >= 0);
            tgt[m] = 0;
            printf("l %s -> %s\n", p, tgt);
        } else if (k == '-') {
            printf("- %04o %ld %s\n", (unsigned)(st.st_mode & 07777), (long)st.st_size, p);
        } else {
            printf("%c %04o %s\n", k, (unsigned)(st.st_mode & 07777), p);
            if (k == 'd') dump_tree(p);
        }
        free(p);
    }
    ls_free(v, n);
}


typedef struct {
    dev_t dev;
    ino_t ino;
} Id;

static Id ident(const char *path) {
    struct stat st;
    CHECK(lstat(path, &st) == 0);
    Id i = {st.st_dev, st.st_ino};
    return i;
}

static int same(Id a, Id b) { return a.dev == b.dev && a.ino == b.ino; }

int main(void) {
    umask(022);
    mkd("inbox");
    mkd("archive");
    mkd("archive/2024");
    put("inbox/a.txt", "alpha");
    put("inbox/b.txt", "bravo!");
    mkd("inbox/proj");
    mkd("inbox/proj/src");
    put("inbox/proj/src/main.c", "int main(void){return 0;}");
    put("inbox/proj/README", "readme");
    CHECK(symlink("src/main.c", "inbox/proj/entry") == 0);

    Id a0 = ident("inbox/a.txt");
    Id p0 = ident("inbox/proj");
    Id m0 = ident("inbox/proj/src/main.c");

    /* file moves keep identity and content */
    CHECK(rename("inbox/a.txt", "archive/2024/a.txt") == 0);
    Id a1 = ident("archive/2024/a.txt");
    CHECK(same(a0, a1));
    CHECK(!exists_nofollow("inbox/a.txt"));
    printf("file move preserved inode: yes, size=%ld\n", fsize("archive/2024/a.txt"));

    /* holding an fd across the rename */
    int fd = open("inbox/b.txt", O_RDWR);
    CHECK(fd >= 0);
    CHECK(rename("inbox/b.txt", "archive/renamed_b.txt") == 0);
    CHECK(write(fd, "??", 2) == 2);
    char buf[16];
    CHECK(pread(fd, buf, sizeof buf, 0) == 6);
    CHECK(close(fd) == 0);
    size_t n;
    unsigned char *c = slurp("archive/renamed_b.txt", &n);
    CHECK(c && n == 6);
    printf("write via old fd landed in moved file: %s\n", (char *)c);
    free(c);

    /* whole subtree moves in one call: nothing below it is touched */
    CHECK(rename("inbox/proj", "archive/2024/proj-v1") == 0);
    CHECK(same(p0, ident("archive/2024/proj-v1")));
    CHECK(same(m0, ident("archive/2024/proj-v1/src/main.c")));
    printf("subtree move kept every inode below it: yes\n");

    /* the relative symlink inside still resolves because it is relative */
    char t[64];
    ssize_t k = readlink("archive/2024/proj-v1/entry", t, sizeof t - 1);
    CHECK(k > 0);
    t[k] = 0;
    int fd2 = open("archive/2024/proj-v1/entry", O_RDONLY);
    CHECK(fd2 >= 0);
    close(fd2);
    printf("relative link %s still resolves after the move: yes\n", t);

    /* ".." of the moved directory is now the new parent: verify via a relative walk */
    struct stat up, exp_parent;
    CHECK(stat("archive/2024/proj-v1/..", &up) == 0);
    CHECK(stat("archive/2024", &exp_parent) == 0);
    CHECK(up.st_ino == exp_parent.st_ino);
    printf("proj-v1/.. is the new parent: yes\n");

    /* renameat across two directory fds */
    int d1 = open("archive", O_RDONLY | O_DIRECTORY);
    int d2 = open("inbox", O_RDONLY | O_DIRECTORY);
    CHECK(d1 >= 0 && d2 >= 0);
    CHECK(renameat(d1, "renamed_b.txt", d2, "b_again.txt") == 0);
    CHECK(renameat(d1, "2024/proj-v1/README", d2, "README.moved") == 0);
    close(d1);
    close(d2);

    dump_tree(".");
    CHECK(rm_rf("inbox") == 0 && rm_rf("archive") == 0);
    return 0;
}
