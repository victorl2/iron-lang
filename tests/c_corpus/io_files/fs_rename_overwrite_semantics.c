/*
 * title: rename over existing targets, file and directory matrix
 * topic: io_files
 * covers: rename, replacing files, EISDIR, ENOTDIR, ENOTEMPTY, EINVAL moving a directory into itself, same-file rename, hard-linked names, symlink replacement
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


static const char *cls(int e) {
    if (e == EEXIST || e == ENOTEMPTY) return "ENOTEMPTY_OR_EEXIST";
    return en(e);
}

static void mv(const char *from, const char *to) {
    errno = 0;
    int rc = rename(from, to);
    int e = errno;
    printf("rename %-9s -> %-9s : %s\n", from, to, rc == 0 ? "OK" : cls(e));
}

static void content(const char *path) {
    size_t n;
    unsigned char *b = slurp(path, &n);
    if (!b) printf("  %s: absent\n", path);
    else {
        printf("  %s: \"%s\"\n", path, (char *)b);
        free(b);
    }
}

int main(void) {
    umask(022);
    put("f1", "one");
    put("f2", "two");
    mkd("d1");
    put("d1/inside", "in1");
    mkd("d2");
    mkd("full");
    put("full/x", "x");

    /* file over file replaces atomically */
    mv("f1", "f2");
    content("f1");
    content("f2");

    /* file over directory, directory over file */
    put("f3", "three");
    mv("f3", "d2");
    mv("d1", "f2");
    CHECK(exists_nofollow("f3") && exists_nofollow("d1") && exists_nofollow("d2"));

    /* directory over empty directory works, over non-empty fails */
    mv("d1", "d2");
    CHECK(!exists_nofollow("d1"));
    content("d2/inside");
    mkd("d3");
    mv("d3", "full");
    mv("d3", "d2");
    CHECK(exists_nofollow("d3"));

    /* directory into its own subtree */
    mkd("tree");
    mkd("tree/sub");
    mv("tree", "tree/sub/moved");
    mv("tree", "tree/self");
    CHECK(exists_nofollow("tree/sub"));

    /* renaming to itself is a successful no-op */
    mv("f2", "f2");
    content("f2");
    mv("d3", "d3");

    /* two hard links to one inode: rename(a,b) with both names is a no-op and keeps both */
    put("ha", "shared");
    CHECK(link("ha", "hb") == 0);
    mv("ha", "hb");
    CHECK(exists_nofollow("ha") && exists_nofollow("hb"));
    struct stat st;
    CHECK(stat("ha", &st) == 0);
    printf("  both names survive, nlink=%ld\n", (long)st.st_nlink);

    /* overwriting a hard link decrements the other side's link count */
    put("other", "zzz");
    mv("other", "ha");
    CHECK(stat("hb", &st) == 0);
    printf("  after replacing ha: hb nlink=%ld ", (long)st.st_nlink);
    content("hb");
    CHECK(st.st_nlink == 1);

    /* renaming a symlink moves the link, not the target; replacing one drops only the link */
    put("real", "real-data");
    CHECK(symlink("real", "sl") == 0);
    put("victim", "victim-data");
    CHECK(symlink("victim", "sl2") == 0);
    mv("sl", "sl2");
    content("real");
    content("victim");
    char tgt[32];
    ssize_t n = readlink("sl2", tgt, sizeof tgt - 1);
    CHECK(n >= 0);
    tgt[n] = 0;
    printf("  sl2 now points at %s, sl exists=%d\n", tgt, exists_nofollow("sl"));

    /* missing source, bad parent */
    mv("nothing", "f9");
    mv("f2", "nodir/f2");
    mv("f2/child", "f9");
    content("f2");

    static const char *all[] = {"f1", "f2", "f3", "d2", "d3", "full", "tree", "ha", "hb", "real",
                                "sl2", "victim"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) CHECK(rm_rf(all[i]) == 0);
    CHECK(!exists_nofollow("d1"));
    return 0;
}
