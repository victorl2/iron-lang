/*
 * title: nftw with FTW_PHYS, FTW_DEPTH and early termination
 * topic: io_files
 * covers: nftw, FTW_PHYS, FTW_DEPTH, typeflag names, ftwbuf level and base, dangling symlinks, callback abort value
 * deps: libc, posix
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <ftw.h>
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


typedef struct {
    char path[96];
    const char *flag;
    int level;
    int base_ok;
    long size;
} Rec;

static Rec recs[64];
static int nrecs;
static int stop_after; /* callback returns 7 once this many entries were seen (0 = never) */

static const char *flag_name(int f) {
    switch (f) {
    case FTW_F: return "FTW_F";
    case FTW_D: return "FTW_D";
    case FTW_DNR: return "FTW_DNR";
    case FTW_DP: return "FTW_DP";
    case FTW_NS: return "FTW_NS";
    case FTW_SL: return "FTW_SL";
    case FTW_SLN: return "FTW_SLN";
    default: return "FTW_?";
    }
}

static int cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    CHECK(nrecs < 64);
    Rec *r = &recs[nrecs++];
    snprintf(r->path, sizeof r->path, "%s", path);
    r->flag = flag_name(flag);
    r->level = ftw->level;
    const char *slash = strrchr(path, '/');
    r->base_ok = (slash ? (int)(slash - path) + 1 : 0) == ftw->base;
    r->size = (flag == FTW_F) ? (long)sb->st_size : -1;
    if (stop_after && nrecs == stop_after) return 7;
    return 0;
}

static int cmp_rec(const void *a, const void *b) {
    return strcmp(((const Rec *)a)->path, ((const Rec *)b)->path);
}

static int index_of(const char *p) {
    for (int i = 0; i < nrecs; i++)
        if (!strcmp(recs[i].path, p)) return i;
    return -1;
}

static void run(const char *label, int flags) {
    nrecs = 0;
    stop_after = 0;
    int rc = nftw("t", cb, 8, flags);
    CHECK(rc == 0);
    /* walk-order property before we sort: parents precede children (pre-order)
     * or follow them (FTW_DEPTH) */
    int depth_first = (flags & FTW_DEPTH) != 0;
    int order_ok = 1;
    for (int i = 0; i < nrecs; i++) {
        char parent[96];
        snprintf(parent, sizeof parent, "%s", recs[i].path);
        char *s = strrchr(parent, '/');
        if (!s) continue;
        *s = 0;
        int pi = index_of(parent);
        if (pi < 0) continue;
        if (depth_first ? pi < i : pi > i) order_ok = 0;
    }
    CHECK(order_ok);
    qsort(recs, (size_t)nrecs, sizeof recs[0], cmp_rec);
    printf("== %s: %d entries, parent order %s\n", label, nrecs,
           depth_first ? "after children" : "before children");
    for (int i = 0; i < nrecs; i++) {
        CHECK(recs[i].base_ok);
        printf("%-8s L%d %s", recs[i].flag, recs[i].level, recs[i].path);
        if (recs[i].size >= 0) printf(" (%ld bytes)", recs[i].size);
        printf("\n");
    }
}

int main(void) {
    umask(022);
    mkd("t");
    mkd("t/a");
    mkd("t/a/deep");
    mkd("t/b");
    mkd("t/empty");
    put("t/root.txt", "12345");
    put("t/a/x", "xx");
    put("t/a/deep/y", "yyyy");
    put("t/b/z", "");
    mkd("outside");
    put("outside/o1", "o1");
    CHECK(symlink("../../outside", "t/b/to_a") == 0); /* directory symlink to a tree outside t */
    CHECK(symlink("../root.txt", "t/b/to_root") == 0);
    CHECK(symlink("missing", "t/dangling") == 0);

    run("FTW_PHYS", FTW_PHYS);
    run("FTW_PHYS|FTW_DEPTH", FTW_PHYS | FTW_DEPTH);
    run("follow links", 0);

    /* a nonzero callback return aborts the walk and becomes nftw's result */
    nrecs = 0;
    stop_after = 4;
    int rc = nftw("t", cb, 8, FTW_PHYS);
    printf("aborted walk: rc=%d visited=%d\n", rc, nrecs);
    CHECK(rc == 7 && nrecs == 4);

    errno = 0;
    nrecs = 0;
    stop_after = 0;
    rc = nftw("no_such_dir", cb, 8, FTW_PHYS);
    printf("missing root: rc=%d err=%s visited=%d\n", rc, en(errno), nrecs);
    CHECK(rc == -1);

    CHECK(rm_rf("t") == 0 && rm_rf("outside") == 0);
    return 0;
}
