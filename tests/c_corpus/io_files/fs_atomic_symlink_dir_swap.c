/*
 * title: Atomic release switch via symlink rename vs three-step directory swap
 * topic: io_files
 * covers: rename of a symlink over a symlink, atomic replacement observed by a concurrent reader process, non-atomic directory exchange gap, releases directory pruning, fork and waitpid
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


static void make_release(int n) {
    char d[64], p[96], v[32];
    snprintf(d, sizeof d, "releases/r%d", n);
    mkd(d);
    snprintf(v, sizeof v, "r%d\n", n);
    snprintf(p, sizeof p, "%s/VERSION", d);
    put(p, v);
    snprintf(p, sizeof p, "%s/DATA", d);
    put(p, v); /* must always match VERSION for a consistent reader */
}

/* point "current" at releases/rN without ever leaving it missing */
static void switch_to(int n) {
    char target[64];
    snprintf(target, sizeof target, "releases/r%d", n);
    (void)unlink("current.tmp");
    CHECK(symlink(target, "current.tmp") == 0);
    CHECK(rename("current.tmp", "current") == 0);
}

static int current_version(void) {
    char buf[16];
    ssize_t k = readlink("current", buf, sizeof buf - 1);
    if (k < 0) return -1;
    buf[k] = 0;
    return atoi(buf + strlen("releases/r"));
}

static int read_at(int dfd, const char *name, char *buf) {
    int fd = openat(dfd, name, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, 15);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

/* reader: resolve "current" once per round (macOS may fail a lookup transiently while the link is
 * being replaced, so a failed lookup is retried), then read both files relative to that directory
 * fd: they must always belong to the same release. Returns 0 if all rounds were consistent. */
static int reader(void) {
    int good = 0;
    for (int i = 0; i < 200000 && good < 3000; i++) {
        int dfd = open("current", O_RDONLY | O_DIRECTORY);
        if (dfd < 0) continue;
        char v[16], d[16];
        int n1 = read_at(dfd, "VERSION", v), n2 = read_at(dfd, "DATA", d);
        close(dfd);
        if (n1 != 3 || n2 != 3 || v[0] != 'r' || strcmp(v, d) != 0) return 1;
        good++;
    }
    return good >= 3000 ? 0 : 2;
}

int main(void) {
    umask(022);
    mkd("releases");
    for (int i = 1; i <= 3; i++) make_release(i);
    CHECK(symlink("releases/r1", "current") == 0);
    printf("initial current -> r%d\n", current_version());

    switch_to(2);
    printf("after switch: current -> r%d, tmp link left behind=%d\n", current_version(), exists_nofollow("current.tmp"));
    CHECK(current_version() == 2);

    /* concurrent reader while the parent flips between r1..r3 */
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(reader());
    int flips = 0, status = 0;
    for (;;) {
        switch_to(1 + flips % 3);
        flips++;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        CHECK(w == 0);
        if (flips > 200000) {
            CHECK(waitpid(pid, &status, 0) == pid);
            break;
        }
    }
    CHECK(WIFEXITED(status));
    printf("reader saw only complete releases during the flips: %s\n", WEXITSTATUS(status) == 0 ? "yes" : "NO");
    CHECK(WEXITSTATUS(status) == 0 && flips >= 1);
    switch_to(3);
    CHECK(current_version() == 3);

    /* the non-atomic alternative: swap two real directories with three renames and watch the gap */
    mkd("live");
    put("live/id", "old");
    mkd("staging");
    put("staging/id", "new");
    CHECK(rename("live", "live.old") == 0);
    struct stat st;
    int gap = stat("live", &st) < 0 && errno == ENOENT;
    printf("between the two renames, 'live' is missing: %s\n", gap ? "yes" : "no");
    CHECK(rename("staging", "live") == 0);
    CHECK(rename("live.old", "staging") == 0);
    size_t n;
    unsigned char *b = slurp("live/id", &n);
    CHECK(b && !strcmp((char *)b, "new"));
    free(b);
    b = slurp("staging/id", &n);
    CHECK(b && !strcmp((char *)b, "old"));
    free(b);
    printf("after the swap: live=new staging=old\n");

    /* renaming a directory over an empty directory is atomic too, but fails if it has content */
    mkd("slot");
    errno = 0;
    CHECK(rename("staging", "slot") == 0);
    CHECK(!exists_nofollow("staging"));
    mkd("staging");
    put("staging/x", "x");
    errno = 0;
    int rc = rename("staging", "live");
    printf("rename onto non-empty dir: %s\n", rc == 0 ? "OK" : (errno == ENOTEMPTY || errno == EEXIST) ? "ENOTEMPTY_OR_EEXIST" : en(errno));
    CHECK(rc < 0);

    /* prune all but the newest two releases */
    int cnt;
    char **v = ls_dir("releases", &cnt);
    int cur = current_version();
    int removed = 0;
    for (int i = 0; i < cnt; i++) {
        int id = atoi(v[i] + 1);
        if (id == cur || id == cur - 1) continue;
        char p[64];
        snprintf(p, sizeof p, "releases/%s", v[i]);
        CHECK(rm_rf(p) == 0);
        removed++;
    }
    ls_free(v, cnt);
    v = ls_dir("releases", &cnt);
    printf("pruned %d release(s); kept:", removed);
    for (int i = 0; i < cnt; i++) printf(" %s", v[i]);
    printf("\n");
    ls_free(v, cnt);
    CHECK(rm_rf("releases") == 0 && rm_rf("current") == 0 && rm_rf("live") == 0 && rm_rf("staging") == 0 && rm_rf("slot") == 0);
    return 0;
}
