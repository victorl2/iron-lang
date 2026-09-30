/*
 * title: Hard links and st_nlink bookkeeping
 * topic: io_files
 * covers: link, unlink, st_nlink, shared inode content, open fd keeps data after last unlink, EEXIST, link to directory refused, EXDEV-free same dir tree
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


static long nlink_of(const char *p) {
    struct stat st;
    if (lstat(p, &st) < 0) return -1;
    return (long)st.st_nlink;
}

int main(void) {
    umask(022);
    put("orig", "0123456789");
    printf("start: orig nlink=%ld\n", nlink_of("orig"));

    static const char *names[] = {"l1", "l2", "l3"};
    for (int i = 0; i < 3; i++) {
        CHECK(link("orig", names[i]) == 0);
        printf("after link %s: nlink=%ld\n", names[i], nlink_of("orig"));
    }
    for (int i = 0; i < 3; i++) CHECK(nlink_of(names[i]) == 4);

    /* a write through one name is visible through all */
    int fd = open("l2", O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3 && close(fd) == 0);
    for (int i = 0; i < 3; i++) CHECK(fsize(names[i]) == 13);
    CHECK(fsize("orig") == 13);
    printf("append via l2, size via orig = %ld\n", fsize("orig"));

    /* chmod is also shared */
    CHECK(chmod("l3", 0600) == 0);
    struct stat st;
    CHECK(stat("orig", &st) == 0);
    printf("chmod via l3 -> orig mode %04o\n", (unsigned)(st.st_mode & 0777));

    /* unlink names one by one */
    CHECK(unlink("orig") == 0);
    printf("after unlink orig: l1 nlink=%ld, orig exists=%d\n", nlink_of("l1"), exists_nofollow("orig"));
    CHECK(unlink("l1") == 0);
    printf("after unlink l1: l2 nlink=%ld\n", nlink_of("l2"));

    /* l2 is the last name of the original inode but l3 shares it: nlink stays 1 */
    CHECK(unlink("l2") == 0);
    put("solo", "0123456789abc");
    /* only name gone but fd open: data still readable, nlink 0 */
    int rd = open("solo", O_RDONLY);
    CHECK(rd >= 0);
    CHECK(unlink("solo") == 0);
    CHECK(fstat(rd, &st) == 0);
    printf("unlinked-but-open: nlink=%ld size=%ld\n", (long)st.st_nlink, (long)st.st_size);
    char buf[32];
    ssize_t n = pread(rd, buf, sizeof buf - 1, 0);
    CHECK(n == 13);
    buf[n] = 0;
    printf("still readable: %s\n", buf);
    CHECK(close(rd) == 0);
    printf("l3 nlink=%ld (l2 removed earlier)\n", nlink_of("l3"));

    /* error cases */
    errno = 0;
    CHECK(link("l3", "l3") < 0);
    printf("link onto itself: %s\n", en(errno));
    put("other", "o");
    errno = 0;
    CHECK(link("l3", "other") < 0);
    printf("link onto existing file: %s\n", en(errno));
    errno = 0;
    CHECK(link("ghost", "g2") < 0);
    printf("link from missing: %s\n", en(errno));
    mkd("adir");
    errno = 0;
    int rc = link("adir", "adir_link");
    int e = errno;
    printf("link to directory: %s\n", rc == 0 ? "OK" : (e == EPERM || e == EACCES) ? "EPERM_OR_EACCES" : en(e));
    CHECK(rc < 0);
    errno = 0;
    CHECK(link("l3", "adir/../nodir/x") < 0);
    printf("link into missing dir: %s\n", en(errno));

    /* symlinks are not hard links: nlink of the target does not change */
    CHECK(symlink("l3", "sym") == 0);
    printf("after symlink: l3 nlink=%ld\n", nlink_of("l3"));

    /* many links: build a fan of 20 names in a subdirectory and tear it down */
    mkd("fan");
    for (int i = 0; i < 20; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "fan/n%02d", i);
        CHECK(link("l3", nm) == 0);
    }
    printf("fan of 20: l3 nlink=%ld\n", nlink_of("l3"));
    int cnt;
    char **v = ls_dir("fan", &cnt);
    for (int i = 0; i < cnt; i += 2) {
        char nm[64];
        snprintf(nm, sizeof nm, "fan/%s", v[i]);
        CHECK(unlink(nm) == 0);
    }
    ls_free(v, cnt);
    printf("after removing half: nlink=%ld\n", nlink_of("l3"));
    CHECK(nlink_of("l3") == 11);

    static const char *all[] = {"l3", "other", "adir", "sym", "fan"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) CHECK(rm_rf(all[i]) == 0);
    return 0;
}
