/*
 * title: Path edge cases through real syscalls
 * topic: io_files
 * covers: empty path, trailing slashes, dot and dotdot components, NAME_MAX boundary, over-long paths, deep nesting via chdir, open flags on directories, unusual but legal file names
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


static const char *r(int rc) { return rc < 0 ? en(errno) : "OK"; }

int main(void) {
    umask(022);
    put("file", "data");
    mkd("dir");
    struct stat st;

    printf("stat(\"\")           : %s\n", r(stat("", &st)));
    printf("stat(\"file\")       : %s\n", r(stat("file", &st)));
    printf("stat(\"file/\")      : %s\n", r(stat("file/", &st)));
    printf("stat(\"file/.\")     : %s\n", r(stat("file/.", &st)));
    printf("stat(\"file/..\")    : %s\n", r(stat("file/..", &st)));
    printf("stat(\"dir/\")       : %s\n", r(stat("dir/", &st)));
    printf("stat(\"dir//\")      : %s\n", r(stat("dir//", &st)));
    printf("stat(\"dir/.\")      : %s\n", r(stat("dir/.", &st)));
    printf("stat(\"dir/./.\")    : %s\n", r(stat("dir/./.", &st)));
    printf("stat(\"dir/..\")     : %s\n", r(stat("dir/..", &st)));
    printf("stat(\"nodir/..\")   : %s\n", r(stat("nodir/..", &st)));
    printf("stat(\"./file\")     : %s\n", r(stat("./file", &st)));
    printf("stat(\".//file\")    : %s\n", r(stat(".//file", &st)));

    /* dir/../file reaches the sibling file via the directory */
    struct stat a, b;
    CHECK(stat("dir/../file", &a) == 0 && stat("file", &b) == 0 && a.st_ino == b.st_ino);
    printf("dir/../file is file: yes\n");

    errno = 0;
    printf("mkdir(\"dir\")       : %s\n", r(mkdir("dir", 0755)));
    printf("mkdir(\"dir/\")      : %s\n", r(mkdir("dir/", 0755)));
    printf("mkdir(\"sub/\")      : %s\n", r(mkdir("sub/", 0755)));
    printf("mkdir(\"sub2//\")    : %s\n", r(mkdir("sub2//", 0755)));
    printf("mkdir(\"file/x\")    : %s\n", r(mkdir("file/x", 0755)));
    printf("mkdir(\"\")          : %s\n", r(mkdir("", 0755)));
    printf("chdir(\"\")          : %s\n", r(chdir("")));

    /* opening directories */
    int fd = open("dir", O_RDONLY);
    printf("open(dir, RDONLY)  : %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd >= 0);
    char c;
    errno = 0;
    ssize_t n = read(fd, &c, 1);
    printf("read(dir fd)       : %s\n", n < 0 ? en(errno) : "OK");
    CHECK(n < 0);
    close(fd);
    errno = 0;
    fd = open("dir", O_WRONLY);
    printf("open(dir, WRONLY)  : %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd < 0);
    errno = 0;
    fd = open("dir", O_RDONLY | O_CREAT | O_EXCL, 0644);
    printf("open(dir, CREAT|EXCL): %s\n", fd < 0 ? en(errno) : "OK");
    errno = 0;
    fd = open("file", O_RDONLY | O_DIRECTORY);
    printf("open(file, O_DIRECTORY): %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd < 0);
    errno = 0;
    fd = open("file/", O_RDONLY);
    printf("open(\"file/\")      : %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd < 0);

    /* name length limits: 255 is fine, 256 is not */
    char name[300];
    memset(name, 'n', sizeof name);
    name[255] = 0;
    fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
    printf("create 255-char name: %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd >= 0);
    close(fd);
    CHECK(unlink(name) == 0);
    name[255] = 'n';
    name[256] = 0;
    errno = 0;
    fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0644);
    printf("create 256-char name: %s\n", fd < 0 ? en(errno) : "OK");
    CHECK(fd < 0);
    /* a path of thousands of short components is longer than PATH_MAX on every platform */
    static char big[6000];
    size_t o = 0;
    while (o + 2 < sizeof big) {
        big[o++] = 'a';
        big[o++] = '/';
    }
    big[o] = 0;
    errno = 0;
    printf("stat(6000-char path): %s\n", r(stat(big, &st)));

    /* deep nesting: descend 150 levels with chdir, write a file, climb back up removing */
    int home = open(".", O_RDONLY | O_DIRECTORY);
    CHECK(home >= 0);
    for (int i = 0; i < 150; i++) {
        CHECK(mkdir("d", 0755) == 0 && chdir("d") == 0);
    }
    put("bottom.txt", "deep");
    int depth = 0;
    /* relative path with 150 components is only ~300 bytes, so it is usable from home */
    CHECK(fchdir(home) == 0);
    char deep[512];
    size_t dl = 0;
    for (int i = 0; i < 150; i++) {
        deep[dl++] = 'd';
        deep[dl++] = '/';
    }
    strcpy(deep + dl, "bottom.txt");
    printf("deep path length %zu bytes, size of bottom.txt = %ld\n", strlen(deep), fsize(deep));
    for (int i = 0; i < 150; i++) {
        CHECK(chdir("d") == 0);
        depth++;
    }
    CHECK(unlink("bottom.txt") == 0);
    for (int i = 0; i < 150; i++) {
        CHECK(chdir("..") == 0 && rmdir("d") == 0);
        depth--;
    }
    CHECK(depth == 0);
    CHECK(fchdir(home) == 0);
    close(home);
    CHECK(!exists_nofollow("d"));
    printf("nesting depth 150 built and removed\n");

    /* names that look odd but are legal */
    static const char *odd[] = {" leading space", "trailing space ", "new\nline", "back\\slash", "...", "-rf",
                                "semi;colon", "q?mark", "star*", "\xe2\x82\xac.txt", "tab\tname", "'quoted'"};
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) put(odd[i], "x");
    int cnt;
    char **v = ls_dir(".", &cnt);
    int found = 0;
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++)
        for (int j = 0; j < cnt; j++)
            if (!strcmp(v[j], odd[i])) found++;
    printf("odd names created and listed: %d of %zu\n", found, sizeof odd / sizeof odd[0]);
    CHECK(found == (int)(sizeof odd / sizeof odd[0]));
    for (int j = 0; j < cnt; j++) CHECK(rm_rf(v[j]) == 0);
    ls_free(v, cnt);
    return 0;
}
