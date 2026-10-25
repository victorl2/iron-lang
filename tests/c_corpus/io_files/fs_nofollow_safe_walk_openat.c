/*
 * title: Symlink-safe tree walk with openat and O_NOFOLLOW
 * topic: io_files
 * covers: openat O_NOFOLLOW|O_DIRECTORY, fdopendir on a dirfd, fstatat AT_SYMLINK_NOFOLLOW, ELOOP on a swapped-in symlink, unsafe path-based walk following links out of the tree
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
    long files, dirs, links, bytes, skipped;
} Sum;

/* walk relative to a directory fd; symlinks are counted, never followed */
static void safe_walk(int dfd, Sum *s) {
    DIR *d = fdopendir(dup(dfd));
    CHECK(d != NULL);
    char *names[64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        CHECK(n < 64);
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        struct stat st;
        CHECK(fstatat(dfd, names[i], &st, AT_SYMLINK_NOFOLLOW) == 0);
        if (S_ISLNK(st.st_mode)) s->links++;
        else if (S_ISREG(st.st_mode)) {
            s->files++;
            s->bytes += (long)st.st_size;
        } else if (S_ISDIR(st.st_mode)) {
            int sub = openat(dfd, names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (sub < 0) {
                CHECK(errno == ELOOP || errno == ENOTDIR || errno == EACCES);
                s->skipped++; /* raced into something else (or unreadable): do not follow */
            } else {
                s->dirs++;
                safe_walk(sub, s);
                close(sub);
            }
        }
        free(names[i]);
    }
}

/* the naive version: stat() follows symlinks, so a link to a directory is walked as a directory */
static void naive_walk(const char *path, Sum *s, int depth) {
    if (depth > 6) return; /* guard against cycles for the demonstration */
    int n;
    char **v = ls_dir(path, &n);
    CHECK(v);
    for (int i = 0; i < n; i++) {
        char *p = join2(path, v[i]);
        struct stat st;
        if (stat(p, &st) == 0) {
            if (S_ISREG(st.st_mode)) {
                s->files++;
                s->bytes += (long)st.st_size;
            } else if (S_ISDIR(st.st_mode)) {
                s->dirs++;
                naive_walk(p, s, depth + 1);
            }
        }
        free(p);
    }
    ls_free(v, n);
}

/* open a relative path one component at a time, refusing symlinks anywhere on the way */
static int open_nofollow_path(int rootfd, const char *rel, int *err) {
    int cur = dup(rootfd);
    const char *p = rel;
    while (*p) {
        char comp[64];
        size_t l = strcspn(p, "/");
        CHECK(l < sizeof comp);
        memcpy(comp, p, l);
        comp[l] = 0;
        const char *next = p + l;
        while (*next == '/') next++;
        int last = *next == 0;
        int nfd = openat(cur, comp, O_RDONLY | O_NOFOLLOW | (last ? 0 : O_DIRECTORY));
        int e = errno;
        close(cur);
        if (nfd < 0) {
            *err = e;
            return -1;
        }
        cur = nfd;
        p = next;
    }
    return cur;
}

int main(void) {
    umask(022);
    mkd("site");
    mkd("site/a");
    mkd("site/a/b");
    put("site/a/one.txt", "one");
    put("site/a/b/two.txt", "twotwo");
    put("site/top.txt", "top!");
    mkd("outside");
    mkd("outside/deep");
    put("outside/secret1", "0123456789");
    put("outside/deep/secret2", "abcdefghijklmnopqrstuvwxyz");
    CHECK(symlink("../../outside", "site/a/escape") == 0);   /* directory link out of the tree */
    CHECK(symlink("../outside/secret1", "site/leak") == 0);  /* file link out of the tree */
    CHECK(symlink("a", "site/self_alias") == 0);            /* directory link inside the tree */

    Sum safe = {0}, naive = {0};
    int root = open("site", O_RDONLY | O_DIRECTORY);
    CHECK(root >= 0);
    safe_walk(root, &safe);
    naive_walk("site", &naive, 0);
    printf("safe : files=%ld dirs=%ld links=%ld bytes=%ld skipped=%ld\n", safe.files, safe.dirs, safe.links,
           safe.bytes, safe.skipped);
    printf("naive: files=%ld dirs=%ld bytes=%ld (links followed out of the tree)\n", naive.files, naive.dirs,
           naive.bytes);
    CHECK(safe.files == 3 && safe.dirs == 2 && safe.links == 3 && safe.bytes == 3 + 6 + 4);
    CHECK(naive.bytes > safe.bytes && naive.files > safe.files);

    /* component-wise open: plain files work, any symlink on the path is refused */
    static const char *paths[] = {"a/b/two.txt", "top.txt", "a/escape/secret1", "leak", "self_alias/one.txt", "a/nothing"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        int err = 0;
        int fd = open_nofollow_path(root, paths[i], &err);
        if (fd >= 0) {
            char c[4] = {0};
            CHECK(read(fd, c, 3) >= 1);
            printf("%-20s opened, starts with '%.1s'\n", paths[i], c);
            close(fd);
        } else {
            printf("%-20s refused: %s\n", paths[i], err == ELOOP || err == ENOTDIR ? "ELOOP_OR_ENOTDIR" : en(err));
        }
    }

    /* TOCTOU: swap a checked directory for a symlink; the fd-based descent notices */
    CHECK(rename("site/a/b", "site/a/b_orig") == 0);
    CHECK(symlink("../../outside/deep", "site/a/b") == 0);
    int sub = openat(root, "a/b", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int e = errno;
    printf("openat(a/b) after swap: %s\n", sub >= 0 ? "OK" : (e == ELOOP || e == ENOTDIR) ? "ELOOP_OR_ENOTDIR" : en(e));
    CHECK(sub < 0);
    int plain = open("site/a/b/secret2", O_RDONLY);
    printf("plain open through the swapped link reads outside data: %s\n", plain >= 0 ? "yes" : "no");
    CHECK(plain >= 0);
    close(plain);

    close(root);
    CHECK(rm_rf("site") == 0 && rm_rf("outside") == 0);
    return 0;
}
