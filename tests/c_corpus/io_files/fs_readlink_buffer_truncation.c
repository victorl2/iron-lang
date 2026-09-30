/*
 * title: readlink truncation, sizes and manual hop counting
 * topic: io_files
 * covers: symlink, readlink, readlinkat, no NUL terminator, silent truncation, lstat size equals target length, EINVAL on non-links, chain following
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


static void show(const char *label, const char *buf, ssize_t n) {
    printf("%-10s n=%zd [", label, n);
    for (ssize_t i = 0; i < n; i++) putchar(buf[i]);
    printf("]\n");
}

/* follow a chain of symlinks by hand; returns hops or -1 on a cycle/limit */
static int follow(const char *start, char *final_name, size_t cap, int limit) {
    char cur[256];
    snprintf(cur, sizeof cur, "%s", start);
    for (int hops = 0; hops <= limit; hops++) {
        char tgt[256];
        ssize_t n = readlink(cur, tgt, sizeof tgt - 1);
        if (n < 0) {
            CHECK(errno == EINVAL || errno == ENOENT);
            snprintf(final_name, cap, "%s", cur);
            return errno == EINVAL ? hops : -2;
        }
        tgt[n] = 0;
        snprintf(cur, sizeof cur, "%s", tgt); /* all links here are cwd-relative names */
    }
    return -1;
}

int main(void) {
    umask(022);
    put("target", "T");
    CHECK(symlink("target", "l1") == 0);
    CHECK(symlink("../a/b/../c/target", "l2") == 0);

    char buf[300];
    memset(buf, '#', sizeof buf);
    ssize_t n = readlink("l1", buf, sizeof buf);
    show("l1 full", buf, n);
    CHECK(buf[n] == '#'); /* no NUL written */

    /* truncation is silent: result equals the buffer size */
    for (size_t cap = 1; cap <= 8; cap += 3) {
        memset(buf, '#', 16);
        n = readlink("l2", buf, cap);
        char label[16];
        snprintf(label, sizeof label, "cap=%zu", cap);
        show(label, buf, n);
        CHECK((size_t)n == cap && buf[cap] == '#');
    }

    struct stat st;
    CHECK(lstat("l1", &st) == 0);
    printf("lstat size l1=%ld l2=", (long)st.st_size);
    CHECK((size_t)st.st_size == strlen("target"));
    CHECK(lstat("l2", &st) == 0);
    printf("%ld\n", (long)st.st_size);
    CHECK((size_t)st.st_size == strlen("../a/b/../c/target"));

    /* a 200 byte target */
    char longt[201];
    for (int i = 0; i < 200; i++) longt[i] = (char)('a' + i % 26);
    longt[200] = 0;
    CHECK(symlink(longt, "l_long") == 0);
    n = readlink("l_long", buf, sizeof buf);
    CHECK(n == 200 && memcmp(buf, longt, 200) == 0);
    CHECK(lstat("l_long", &st) == 0 && st.st_size == 200);
    unsigned h = 2166136261u;
    for (ssize_t i = 0; i < n; i++) h = (h ^ (unsigned char)buf[i]) * 16777619u;
    printf("long target length %zd, fnv %08x\n", n, h);

    /* errors */
    errno = 0;
    CHECK(readlink("target", buf, sizeof buf) < 0);
    printf("readlink(regular): %s\n", en(errno));
    errno = 0;
    CHECK(readlink("absent", buf, sizeof buf) < 0);
    printf("readlink(missing): %s\n", en(errno));
    mkd("d");
    errno = 0;
    CHECK(readlink("d", buf, sizeof buf) < 0);
    printf("readlink(dir): %s\n", en(errno));
    errno = 0;
    CHECK(symlink("x", "l1") < 0);
    printf("symlink onto existing: %s\n", en(errno));
    errno = 0;
    CHECK(symlink("x", "no/such/parent/l") < 0);
    printf("symlink in missing dir: %s\n", en(errno));

    /* readlinkat relative to a directory fd */
    CHECK(symlink("sibling", "d/inner") == 0);
    int dfd = open("d", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0);
    n = readlinkat(dfd, "inner", buf, sizeof buf);
    show("readlinkat", buf, n);
    CHECK(n == 7 && memcmp(buf, "sibling", 7) == 0);
    close(dfd);

    /* chains: c0 -> c1 -> c2 -> c3 -> target, and a two-node cycle */
    CHECK(symlink("target", "c3") == 0);
    CHECK(symlink("c3", "c2") == 0);
    CHECK(symlink("c2", "c1") == 0);
    CHECK(symlink("c1", "c0") == 0);
    CHECK(symlink("cyc_b", "cyc_a") == 0);
    CHECK(symlink("cyc_a", "cyc_b") == 0);
    CHECK(symlink("gone", "dead") == 0);
    static const char *starts[] = {"c0", "c2", "target", "cyc_a", "dead"};
    for (size_t i = 0; i < sizeof starts / sizeof starts[0]; i++) {
        char fin[64] = "";
        int hops = follow(starts[i], fin, sizeof fin, 16);
        if (hops >= 0) printf("follow %-6s -> %s after %d hops\n", starts[i], fin, hops);
        else if (hops == -1) printf("follow %-6s -> cycle (gave up)\n", starts[i]);
        else printf("follow %-6s -> broken at %s\n", starts[i], fin);
    }
    /* the kernel agrees with the manual walk */
    int fd = open("c0", O_RDONLY);
    CHECK(fd >= 0);
    char one;
    CHECK(read(fd, &one, 1) == 1 && one == 'T');
    close(fd);
    printf("open(c0) reads through 4 links: %c\n", one);

    static const char *all[] = {"target", "l1", "l2", "l_long", "d", "c0", "c1", "c2", "c3",
                                "cyc_a", "cyc_b", "dead"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) CHECK(rm_rf(all[i]) == 0);
    return 0;
}
