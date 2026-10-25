/*
 * title: Symlink loops: kernel ELOOP vs a userland cycle detector
 * topic: io_files
 * covers: ELOOP from stat and open, O_NOFOLLOW, self-referencing links, mutual cycles, long chains, loop entered from a tail, cycle detection by visited set
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


typedef enum { R_FILE, R_DIR, R_DANGLING, R_LOOP } Res;

static const char *res_name(Res r) {
    switch (r) {
    case R_FILE: return "file";
    case R_DIR: return "dir";
    case R_DANGLING: return "dangling";
    default: return "loop";
    }
}

/* resolve a link inside directory "l/" (targets are plain sibling names) with a visited set */
static Res classify(const char *name, int *hops) {
    char seen[16][32];
    int nseen = 0;
    char cur[32];
    snprintf(cur, sizeof cur, "%s", name);
    *hops = 0;
    for (;;) {
        for (int i = 0; i < nseen; i++)
            if (!strcmp(seen[i], cur)) return R_LOOP;
        CHECK(nseen < 16);
        snprintf(seen[nseen++], sizeof seen[0], "%s", cur);
        char path[64], tgt[64];
        snprintf(path, sizeof path, "l/%s", cur);
        struct stat st;
        if (lstat(path, &st) < 0) return R_DANGLING;
        if (S_ISDIR(st.st_mode)) return R_DIR;
        if (!S_ISLNK(st.st_mode)) return R_FILE;
        ssize_t n = readlink(path, tgt, sizeof tgt - 1);
        CHECK(n > 0);
        tgt[n] = 0;
        snprintf(cur, sizeof cur, "%s", tgt);
        (*hops)++;
    }
}

int main(void) {
    umask(022);
    mkd("l");
    put("l/file", "F");
    mkd("l/dir");
    static const char *links[][2] = {
        {"c1", "c2"},     {"c2", "c3"},     {"c3", "file"},  /* healthy chain */
        {"x", "y"},       {"y", "z"},       {"z", "x"},      /* 3-cycle */
        {"s", "s"},                                          /* self loop */
        {"tail", "x"},                                       /* tail into the cycle */
        {"d1", "d2"},     {"d2", "gone"},                    /* dangling chain */
        {"dl", "dir"},                                       /* directory link */
    };
    for (size_t i = 0; i < sizeof links / sizeof links[0]; i++) {
        char p[64];
        snprintf(p, sizeof p, "l/%s", links[i][0]);
        CHECK(symlink(links[i][1], p) == 0);
    }

    static const char *probe[] = {"file", "c1", "x", "s", "tail", "d1", "dl"};
    for (size_t i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        char p[64];
        snprintf(p, sizeof p, "l/%s", probe[i]);
        int hops;
        Res mine = classify(probe[i], &hops);

        struct stat st;
        errno = 0;
        int rc = stat(p, &st);
        int e = errno;
        errno = 0;
        int fd = open(p, O_RDONLY);
        int e2 = errno;
        if (fd >= 0) close(fd);
        errno = 0;
        int fd3 = open(p, O_RDONLY | O_NOFOLLOW);
        int e3 = errno;
        if (fd3 >= 0) close(fd3);

        /* kernel verdict must match the userland one */
        Res kern = rc == 0 ? (S_ISDIR(st.st_mode) ? R_DIR : R_FILE) : (e == ELOOP ? R_LOOP : R_DANGLING);
        CHECK(kern == mine);
        printf("%-5s userland=%-8s hops=%d stat=%s open=%s open_nofollow=%s\n", probe[i],
               res_name(mine), hops, rc == 0 ? "OK" : en(e), fd >= 0 ? "OK" : en(e2),
               fd3 >= 0 ? "OK" : en(e3));
    }

    /* path components that go through a loop fail with ELOOP too */
    struct stat st;
    errno = 0;
    CHECK(stat("l/x/child", &st) < 0);
    printf("stat(l/x/child): %s\n", en(errno));
    errno = 0;
    CHECK(stat("l/dl/nothing", &st) < 0);
    printf("stat(l/dl/nothing): %s\n", en(errno));
    CHECK(stat("l/dl/.", &st) == 0 && S_ISDIR(st.st_mode));

    /* the loop members are still ordinary symlinks for lstat/readlink/unlink */
    CHECK(lstat("l/s", &st) == 0 && S_ISLNK(st.st_mode) && st.st_size == 1);
    printf("lstat(self loop): link of length %ld\n", (long)st.st_size);

    /* a very long chain hits the kernel's hop limit but the userland walk still terminates */
    mkd("chain");
    put("chain/end", "end");
    char prev[32] = "end";
    for (int i = 0; i < 100; i++) {
        char nm[32], p[64];
        snprintf(nm, sizeof nm, "k%03d", i);
        snprintf(p, sizeof p, "chain/%s", nm);
        CHECK(symlink(prev, p) == 0);
        snprintf(prev, sizeof prev, "%s", nm);
    }
    int ok3 = open("chain/k002", O_RDONLY);
    CHECK(ok3 >= 0);
    close(ok3);
    errno = 0;
    int far = open("chain/k099", O_RDONLY);
    int efar = errno;
    CHECK(far < 0 && efar == ELOOP);
    printf("chain depth 3 opens: yes; depth 100: %s\n", en(efar));
    int hops = 0;
    char cur[32] = "k099";
    for (;;) {
        char p[64], t[64];
        snprintf(p, sizeof p, "chain/%s", cur);
        ssize_t n = readlink(p, t, sizeof t - 1);
        if (n < 0) break;
        t[n] = 0;
        snprintf(cur, sizeof cur, "%s", t);
        hops++;
    }
    printf("manual walk of the long chain: %d hops to %s\n", hops, cur);
    CHECK(rm_rf("l") == 0 && rm_rf("chain") == 0);
    return 0;
}
