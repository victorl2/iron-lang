/*
 * title: realpath implemented by hand and compared with libc
 * topic: io_files
 * covers: component-wise lstat/readlink resolution, symlink expansion into the pending path, physical .. semantics, absolute link targets, ELOOP limit, ENOENT and ENOTDIR reporting, output relative to the scratch root
 * deps: libc, posix
 */
#include <limits.h>
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


#define MAXP 1024

static char root[MAXP]; /* canonical path of the scratch directory */

static int pop_component(char *cur) {
    char *s = strrchr(cur, '/');
    if (!s) return -1;
    if (s == cur) cur[1] = 0; /* stay at "/" */
    else *s = 0;
    return 0;
}

/* returns 0 and fills out, or -1 with errno set */
static int my_realpath(const char *in, char *out) {
    char todo[MAXP], cur[MAXP];
    if (in[0] == '/') {
        snprintf(cur, sizeof cur, "/");
        snprintf(todo, sizeof todo, "%s", in);
    } else {
        snprintf(cur, sizeof cur, "%s", root);
        snprintf(todo, sizeof todo, "%s", in);
    }
    int links = 0;
    char *p = todo;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256];
        size_t l = 0;
        while (p[l] && p[l] != '/') l++;
        CHECK(l < sizeof comp);
        memcpy(comp, p, l);
        comp[l] = 0;
        p += l;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            pop_component(cur);
            continue;
        }
        size_t cl = strlen(cur);
        CHECK(cl + l + 2 < sizeof cur);
        if (cl > 1) cur[cl++] = '/';
        memcpy(cur + cl, comp, l + 1);
        struct stat st;
        if (lstat(cur, &st) < 0) return -1;
        if (S_ISLNK(st.st_mode)) {
            if (++links > 40) {
                errno = ELOOP;
                return -1;
            }
            char tgt[MAXP];
            ssize_t n = readlink(cur, tgt, sizeof tgt - 1);
            CHECK(n > 0);
            tgt[n] = 0;
            pop_component(cur);
            char rest[MAXP];
            snprintf(rest, sizeof rest, "%s/%s", tgt, p);
            snprintf(todo, sizeof todo, "%s", rest);
            p = todo;
            if (tgt[0] == '/') snprintf(cur, sizeof cur, "/");
        } else if (!S_ISDIR(st.st_mode)) {
            /* a non-directory is fine only if nothing follows */
            const char *q = p;
            while (*q == '/' || (*q == '.' && (q[1] == '/' || q[1] == 0))) q++;
            if (*q) {
                errno = ENOTDIR;
                return -1;
            }
        }
    }
    snprintf(out, MAXP, "%s", cur);
    return 0;
}

static const char *show(const char *abs_path, char *buf) {
    size_t rl = strlen(root);
    if (!strcmp(abs_path, root)) return ".";
    if (!strncmp(abs_path, root, rl) && abs_path[rl] == '/') {
        snprintf(buf, 300, "%s", abs_path + rl + 1);
        return buf;
    }
    return "(outside scratch root)";
}

static void probe(const char *in) {
    char mine[MAXP], lib[PATH_MAX];
    errno = 0;
    int r1 = my_realpath(in, mine);
    int e1 = errno;
    errno = 0;
    char *r2 = realpath(in, lib);
    int e2 = errno;
    CHECK((r1 == 0) == (r2 != NULL));
    char b1[300], b2[300];
    if (r1 == 0) {
        CHECK(strcmp(mine, lib) == 0);
        printf("%-22s => %s\n", in, show(mine, b1));
    } else {
        CHECK(e1 == e2);
        printf("%-22s => error %s\n", in, en(e1));
    }
    (void)b2;
}

int main(void) {
    umask(022);
    char tmp[PATH_MAX];
    CHECK(realpath(".", tmp) != NULL);
    snprintf(root, sizeof root, "%s", tmp);

    mkd("d");
    mkd("d/sub");
    mkd("d/sub/deeper");
    mkd("other");
    put("d/file", "f");
    put("other/target.txt", "t");
    CHECK(symlink("sub", "d/to_sub") == 0);
    CHECK(symlink("../../other", "d/sub/up_other") == 0);
    CHECK(symlink("to_sub", "d/chain1") == 0);       /* link to a link */
    CHECK(symlink("chain1/deeper", "d/chain2") == 0);
    CHECK(symlink("file", "d/to_file") == 0);
    CHECK(symlink("nowhere", "d/dangling") == 0);
    CHECK(symlink("loop_b", "d/loop_a") == 0);
    CHECK(symlink("loop_a", "d/loop_b") == 0);
    /* absolute link into the tree, built from the canonical root */
    char abs_t[MAXP + 32];
    snprintf(abs_t, sizeof abs_t, "%s/other/target.txt", root);
    CHECK(symlink(abs_t, "d/abs_link") == 0);
    CHECK(symlink("/", "d/to_fsroot") == 0);

    static const char *cases[] = {
        ".", "d", "d/", "d/sub/../file", "d/to_sub", "d/to_sub/deeper", "d/to_sub/..",
        "d/chain1", "d/chain2", "d/chain2/..", "d/sub/up_other/target.txt",
        "d/sub/up_other/../d/file", "d/to_file", "d/to_file/x", "d/dangling",
        "d/loop_a", "d/abs_link", "d/to_fsroot", "d//./sub///deeper/.", "nothing", "d/absent/x",
        "d/sub/up_other/../../file",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) probe(cases[i]);
    CHECK(rm_rf("d") == 0 && rm_rf("other") == 0);
    return 0;
}
