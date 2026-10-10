/*
 * title: mkdtemp and mkstemp naming, permissions and cleanup
 * topic: io_files
 * covers: mkdtemp, mkstemp, template rewriting in place, name shape validation, 0700 and 0600 modes independent of umask, uniqueness over many calls, ENOENT for a missing parent, recursive cleanup of a session directory
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


static int name_shape_ok(const char *name, const char *prefix, const char *suffix) {
    size_t pl = strlen(prefix), sl = strlen(suffix), l = strlen(name);
    if (l != pl + 6 + sl) return 0;
    if (strncmp(name, prefix, pl) != 0) return 0;
    if (strcmp(name + pl + 6, suffix) != 0) return 0;
    for (size_t i = pl; i < pl + 6; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

int main(void) {
    umask(077); /* mkdtemp/mkstemp modes must not depend on it in a way that widens access */
    mkd("scratch");
    CHECK(chmod("scratch", 0755) == 0);

    /* one directory, in detail */
    char tmpl[] = "scratch/job.XXXXXX";
    char *made = mkdtemp(tmpl);
    CHECK(made == tmpl);
    printf("mkdtemp returned its argument: yes, shape ok=%d\n", name_shape_ok(tmpl + 8, "job.", ""));
    CHECK(name_shape_ok(tmpl + 8, "job.", ""));
    struct stat st;
    CHECK(stat(tmpl, &st) == 0 && S_ISDIR(st.st_mode));
    printf("mode of new dir: %04o\n", (unsigned)(st.st_mode & 0777));
    CHECK((st.st_mode & 0777) == 0700);

    /* mkstemp in it */
    char ftmpl[64];
    snprintf(ftmpl, sizeof ftmpl, "%s/data.XXXXXX", tmpl);
    int fd = mkstemp(ftmpl);
    CHECK(fd >= 0);
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    printf("mkstemp file mode: %04o, size %ld, shape ok=%d\n", (unsigned)(st.st_mode & 0777), (long)st.st_size,
           name_shape_ok(ftmpl + strlen(tmpl) + 1, "data.", ""));
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(write(fd, "payload", 7) == 7);
    CHECK(close(fd) == 0);
    CHECK(fsize(ftmpl) == 7);

    /* many directories: all unique, all correctly shaped */
    char names[64][32];
    for (int i = 0; i < 64; i++) {
        snprintf(names[i], sizeof names[i], "scratch/t_%02d_XXXXXX", i);
        CHECK(mkdtemp(names[i]) != NULL);
    }
    int distinct = 1, shape = 0;
    for (int i = 0; i < 64; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(names[i], names[j])) dup = 1;
        if (dup) distinct = 0;
        char pre[8];
        snprintf(pre, sizeof pre, "t_%02d_", i);
        shape += name_shape_ok(names[i] + 8, pre, "");
    }
    printf("64 mkdtemp calls: all distinct=%d, well-formed=%d\n", distinct, shape);
    CHECK(distinct && shape == 64);
    /* the six random characters differ even when the rest of the template is the same */
    char a[] = "scratch/same.XXXXXX", b[] = "scratch/same.XXXXXX";
    CHECK(mkdtemp(a) && mkdtemp(b));
    CHECK(strcmp(a, b) != 0);
    printf("same template twice gives different names: yes\n");

    /* bad templates leave nothing behind */
    char bad3[] = "scratch/nope/x.XXXXXX";
    errno = 0;
    CHECK(mkdtemp(bad3) == NULL);
    printf("missing parent: %s\n", en(errno));
    CHECK(errno == ENOENT);

    int n;
    char **v = ls_dir("scratch", &n);
    printf("entries in scratch: %d\n", n);
    CHECK(n == 64 + 1 + 2);
    ls_free(v, n);

    /* cleanup of the whole session tree, counting removed entries */
    long removed = 0;
    CHECK(rm_at(AT_FDCWD, "scratch", &removed) == 0);
    printf("removed %ld entries (dirs, files, root)\n", removed);
    CHECK(removed == 64 + 2 + 1 + 1 + 1);
    CHECK(!exists_nofollow("scratch"));
    return 0;
}
