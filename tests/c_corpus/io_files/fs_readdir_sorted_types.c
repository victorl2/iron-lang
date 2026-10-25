/*
 * title: readdir with d_type, sorted output, rewinddir and seekdir
 * topic: io_files
 * covers: opendir, readdir, d_type with lstat fallback, rewinddir, telldir, seekdir, dot entries, awkward file names, strcmp byte order
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

static inline char kind_of(mode_t m) {
    if (S_ISREG(m)) return '-';
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISFIFO(m)) return 'p';
    if (S_ISSOCK(m)) return 's';
    if (S_ISCHR(m)) return 'c';
    if (S_ISBLK(m)) return 'b';
    return '?';
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


static const char *names[] = {
    "alpha.txt", "Beta.txt", ".hidden", "with space", "tab\there", "~tilde", "-dash", "zzz",
    "\xc3\xa9t\xc3\xa9", "0", "10", "9", "a", "B2", "b.d", "sub", "fifo0", "link_to_alpha", "link_dead",
};

static char type_letter_from_dtype(const struct dirent *e, const char *path) {
#ifdef DT_UNKNOWN
    switch (e->d_type) {
    case DT_REG: return '-';
    case DT_DIR: return 'd';
    case DT_LNK: return 'l';
    case DT_FIFO: return 'p';
    default: break;
    }
#endif
    struct stat st;
    CHECK(lstat(path, &st) == 0);
    return kind_of(st.st_mode);
}

int main(void) {
    umask(022);
    mkd("dir");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[64];
        snprintf(p, sizeof p, "dir/%s", names[i]);
        if (!strcmp(names[i], "sub")) mkd(p);
        else if (!strcmp(names[i], "fifo0")) CHECK(mkfifo(p, 0600) == 0);
        else if (!strcmp(names[i], "link_to_alpha")) CHECK(symlink("alpha.txt", p) == 0);
        else if (!strcmp(names[i], "link_dead")) CHECK(symlink("no-such-target", p) == 0);
        else put(p, names[i]);
    }
    /* a name close to NAME_MAX */
    char longname[121];
    memset(longname, 'L', 120);
    longname[120] = 0;
    char lp[200];
    snprintf(lp, sizeof lp, "dir/%s", longname);
    put(lp, "long");

    /* pass 1: raw readdir, count dot entries, collect (unsorted) */
    DIR *d = opendir("dir");
    CHECK(d);
    int dot = 0, dotdot = 0, total = 0;
    char *seen[64];
    char kinds[64];
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".")) dot++;
        else if (!strcmp(e->d_name, "..")) dotdot++;
        else {
            char p[300];
            snprintf(p, sizeof p, "dir/%s", e->d_name);
            CHECK(total < 64);
            kinds[total] = type_letter_from_dtype(e, p);
            seen[total++] = strdup(e->d_name);
        }
    }
    printf("dot=%d dotdot=%d entries=%d\n", dot, dotdot, total);

    /* pass 2: rewinddir must reproduce the same multiset in the same order */
    rewinddir(d);
    int idx = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        CHECK(idx < total && strcmp(seen[idx], e->d_name) == 0);
        idx++;
    }
    CHECK(idx == total);

    /* telldir / seekdir: resume in the middle */
    rewinddir(d);
    int skip = 0;
    const int want_before = 5;
    while (skip < want_before && (e = readdir(d)) != NULL) skip++;
    long pos = telldir(d);
    char *tail1[64];
    int t1 = 0;
    while ((e = readdir(d)) != NULL) tail1[t1++] = strdup(e->d_name);
    seekdir(d, pos);
    int t2 = 0;
    while ((e = readdir(d)) != NULL) {
        CHECK(t2 < t1 && strcmp(tail1[t2], e->d_name) == 0);
        t2++;
    }
    CHECK(t1 == t2);
    printf("seekdir resume replays %d entries: yes\n", t2);
    for (int i = 0; i < t1; i++) free(tail1[i]);
    CHECK(closedir(d) == 0);

    /* sorted listing with kinds, joined from the unsorted pass */
    int n;
    char **v = ls_dir("dir", &n);
    CHECK(n == total);
    for (int i = 0; i < n; i++) {
        char k = '?';
        for (int j = 0; j < total; j++)
            if (!strcmp(seen[j], v[i])) k = kinds[j];
        size_t len = strlen(v[i]);
        if (len > 20) printf("%c <%zu x '%c'>\n", k, len, v[i][0]);
        else {
            printf("%c ", k);
            for (const unsigned char *c = (const unsigned char *)v[i]; *c; c++) {
                if (*c < 32 || *c >= 127) printf("\\x%02x", *c);
                else putchar(*c);
            }
            putchar('\n');
        }
    }
    ls_free(v, n);
    for (int i = 0; i < total; i++) free(seen[i]);

    /* opendir errors */
    errno = 0;
    CHECK(opendir("dir/alpha.txt") == NULL);
    printf("opendir(file): %s\n", en(errno));
    errno = 0;
    CHECK(opendir("nope") == NULL);
    printf("opendir(missing): %s\n", en(errno));
    CHECK(rm_rf("dir") == 0);
    return 0;
}
