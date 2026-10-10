/*
 * title: rm -rf built on openat, fdopendir and unlinkat
 * topic: io_files
 * covers: fdopendir, openat O_NOFOLLOW, unlinkat AT_REMOVEDIR, symlinks not followed, unreadable directory repair, deletion counting
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

/* errno -> stable name (never print strerror text or raw numbers) */
static inline const char *en(int e) {
    switch (e) {
    case 0: return "OK";
    case EEXIST: return "EEXIST";
    case ENOENT: return "ENOENT";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EINVAL: return "EINVAL";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case ELOOP: return "ELOOP";
    case EXDEV: return "EXDEV";
    case EBADF: return "EBADF";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    default: return "EOTHER";
    }
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


typedef struct {
    long files, dirs, links;
} Counts;

/* delete `name` inside directory `pfd`, counting what was removed */
static int rm_rf_at(int pfd, const char *name, Counts *c) {
    struct stat st;
    if (fstatat(pfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        if (unlinkat(pfd, name, 0) < 0) return -1;
        if (S_ISLNK(st.st_mode)) c->links++;
        else c->files++;
        return 0;
    }
    /* directories without rwx for the owner cannot be opened or emptied: restore access first */
    if ((st.st_mode & 0700) != 0700) CHECK(fchmodat(pfd, name, 0700, 0) == 0);
    int fd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    DIR *d = fdopendir(fd);
    CHECK(d != NULL);
    struct dirent *e;
    int rc = 0;
    /* removing entries while iterating is only defined for the entry just returned;
     * rewind after each removal to be safe */
    for (;;) {
        rewinddir(d);
        const char *victim = NULL;
        char buf[64];
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(buf, sizeof buf, "%s", e->d_name);
            victim = buf;
            break;
        }
        if (!victim) break;
        if (rm_rf_at(fd, victim, c) < 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    if (rc == 0 && unlinkat(pfd, name, AT_REMOVEDIR) < 0) rc = -1;
    if (rc == 0) c->dirs++;
    return rc;
}

static void build(void) {
    mkd("victim");
    mkd("victim/a");
    mkd("victim/a/b");
    mkd("victim/a/b/c");
    put("victim/top.txt", "top");
    put("victim/a/one", "1");
    put("victim/a/b/two", "22");
    put("victim/a/b/c/three", "333");
    mkd("victim/locked");
    put("victim/locked/secret", "s");
    CHECK(chmod("victim/locked", 0000) == 0);
    mkd("victim/rox");
    mkd("victim/rox/inner");
    put("victim/rox/inner/f", "f");
    CHECK(chmod("victim/rox", 0500) == 0);
    /* symlinks pointing outside the victim must survive */
    CHECK(symlink("../keep", "victim/out_link") == 0);
    CHECK(symlink("../keep/precious", "victim/a/file_link") == 0);
    CHECK(symlink("nonexistent", "victim/dangling") == 0);
}

int main(void) {
    umask(022);
    mkd("keep");
    put("keep/precious", "do not delete");
    build();

    Counts c = {0, 0, 0};
    CHECK(rm_rf_at(AT_FDCWD, "victim", &c) == 0);
    printf("removed: files=%ld dirs=%ld symlinks=%ld\n", c.files, c.dirs, c.links);
    CHECK(c.files == 6 && c.dirs == 7 && c.links == 3);

    struct stat st;
    CHECK(lstat("victim", &st) < 0 && errno == ENOENT);
    size_t n = 0;
    unsigned char *k = slurp("keep/precious", &n);
    CHECK(k && n == 13 && memcmp(k, "do not delete", 13) == 0);
    free(k);
    printf("symlink targets outside the tree survived: yes\n");

    /* deleting a symlink to a directory removes only the link */
    CHECK(symlink("keep", "lk") == 0);
    Counts c2 = {0, 0, 0};
    CHECK(rm_rf_at(AT_FDCWD, "lk", &c2) == 0);
    printf("removed via dir symlink: files=%ld dirs=%ld symlinks=%ld\n", c2.files, c2.dirs,
           c2.links);
    CHECK(lstat("keep", &st) == 0 && S_ISDIR(st.st_mode));

    /* missing path and a plain file */
    Counts c3 = {0, 0, 0};
    CHECK(rm_rf_at(AT_FDCWD, "ghost", &c3) < 0 && errno == ENOENT);
    put("plain", "p");
    CHECK(rm_rf_at(AT_FDCWD, "plain", &c3) == 0 && c3.files == 1);
    printf("missing -> %s, plain file -> files=%ld\n", en(ENOENT), c3.files);

    int cnt;
    char **v = ls_dir(".", &cnt);
    printf("left in cwd: %d (%s)\n", cnt, v[0]);
    ls_free(v, cnt);
    Counts c4 = {0, 0, 0};
    CHECK(rm_rf_at(AT_FDCWD, "keep", &c4) == 0 && c4.files == 1 && c4.dirs == 1);
    return 0;
}
