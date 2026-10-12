/*
 * title: rsync-style one-way mirror with delete, replace-by-rename and dry run
 * topic: io_files
 * covers: sorted merge of source and destination, create/update/delete/replace/relink/chmod actions, temp-file plus rename updates, type changes file<->dir<->symlink, dry run leaves destination untouched, second run is a no-op
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

static inline char *join2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *r = malloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb + 1);
    return r;
}


static int dry, mute;
static int counts[8];
enum { A_MKDIR, A_COPY, A_UPDATE, A_DELETE, A_RELINK, A_CHMOD, A_REPLACE, A_SKIP };
static const char *aname[] = {"mkdir", "copy", "update", "delete", "relink", "chmod", "replace", "skip"};

static void act(int a, const char *rel) {
    if (mute) return;
    counts[a]++;
    if (a != A_SKIP) printf("  %-7s %s\n", aname[a], rel);
}

static void copy_content(const char *src, const char *dst, mode_t mode) {
    size_t n;
    unsigned char *b = slurp(src, &n);
    CHECK(b);
    /* write next to the target and rename over it so readers never see a partial file */
    char tmp[160];
    snprintf(tmp, sizeof tmp, "%s.sync-tmp", dst);
    CHECK(put_bytes(tmp, b, n) == 0);
    CHECK(chmod(tmp, mode) == 0);
    CHECK(rename(tmp, dst) == 0);
    free(b);
}

static void create_from(const char *src, const char *dst, const char *rel);

static void sync_dir(const char *src, const char *dst, const char *rel) {
    int ns, nd;
    char **vs = ls_dir(src, &ns), **vd = ls_dir(dst, &nd);
    CHECK(vs && vd);
    int i = 0, j = 0;
    while (i < ns || j < nd) {
        int c = i >= ns ? 1 : j >= nd ? -1 : strcmp(vs[i], vd[j]);
        const char *name = c <= 0 ? vs[i] : vd[j];
        char relp[128];
        snprintf(relp, sizeof relp, "%s%s%s", rel, rel[0] ? "/" : "", name);
        char *ps = c <= 0 ? join2(src, vs[i]) : NULL, *pd = c >= 0 ? join2(dst, vd[j]) : NULL;
        if (c < 0) {
            char *d = join2(dst, name);
            create_from(ps, d, relp);
            free(d);
            i++;
        } else if (c > 0) {
            act(A_DELETE, relp);
            if (!dry) CHECK(rm_rf(pd) == 0);
            j++;
        } else {
            struct stat a, b;
            CHECK(lstat(ps, &a) == 0 && lstat(pd, &b) == 0);
            if ((a.st_mode & S_IFMT) != (b.st_mode & S_IFMT)) {
                act(A_REPLACE, relp);
                if (!dry) {
                    CHECK(rm_rf(pd) == 0);
                    mute = 1; /* the replace line already covers everything below it */
                    create_from(ps, pd, relp);
                    mute = 0;
                }
            } else if (S_ISLNK(a.st_mode)) {
                char x[128], y[128];
                ssize_t nx = readlink(ps, x, sizeof x - 1), ny = readlink(pd, y, sizeof y - 1);
                CHECK(nx >= 0 && ny >= 0);
                x[nx] = y[ny] = 0;
                if (strcmp(x, y)) {
                    act(A_RELINK, relp);
                    if (!dry) CHECK(unlink(pd) == 0 && symlink(x, pd) == 0);
                } else act(A_SKIP, relp);
            } else if (S_ISDIR(a.st_mode)) {
                if ((a.st_mode & 07777) != (b.st_mode & 07777)) {
                    act(A_CHMOD, relp);
                    if (!dry) CHECK(chmod(pd, a.st_mode & 07777) == 0);
                }
                sync_dir(ps, pd, relp);
            } else {
                size_t la, lb;
                unsigned char *x = slurp(ps, &la), *y = slurp(pd, &lb);
                int same = x && y && la == lb && memcmp(x, y, la) == 0;
                free(x);
                free(y);
                if (!same) {
                    act(A_UPDATE, relp);
                    if (!dry) copy_content(ps, pd, a.st_mode & 07777);
                } else if ((a.st_mode & 07777) != (b.st_mode & 07777)) {
                    act(A_CHMOD, relp);
                    if (!dry) CHECK(chmod(pd, a.st_mode & 07777) == 0);
                } else act(A_SKIP, relp);
            }
            i++;
            j++;
        }
        free(ps);
        free(pd);
    }
    ls_free(vs, ns);
    ls_free(vd, nd);
}

static void create_from(const char *src, const char *dst, const char *rel) {
    struct stat st;
    CHECK(lstat(src, &st) == 0);
    if (S_ISDIR(st.st_mode)) {
        act(A_MKDIR, rel);
        if (!dry) CHECK(mkdir(dst, 0755) == 0);
        /* recurse against an empty destination: in dry mode the directory does not exist,
         * so list the source alone */
        int n;
        char **v = ls_dir(src, &n);
        for (int i = 0; i < n; i++) {
            char *s = join2(src, v[i]), *d = join2(dst, v[i]), r[160];
            snprintf(r, sizeof r, "%s/%s", rel, v[i]);
            create_from(s, d, r);
            free(s);
            free(d);
        }
        ls_free(v, n);
        if (!dry) CHECK(chmod(dst, st.st_mode & 07777) == 0);
    } else if (S_ISLNK(st.st_mode)) {
        char t[128];
        ssize_t n = readlink(src, t, sizeof t - 1);
        CHECK(n >= 0);
        t[n] = 0;
        act(A_COPY, rel);
        if (!dry) CHECK(symlink(t, dst) == 0);
    } else {
        act(A_COPY, rel);
        if (!dry) copy_content(src, dst, st.st_mode & 07777);
    }
}

/* equality of two trees: names, types, modes, sizes, bytes, link texts */
static int same_tree(const char *a, const char *b) {
    struct stat sa, sb;
    if (lstat(a, &sa) < 0 || lstat(b, &sb) < 0) return 0;
    if ((sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) return 0;
    if (S_ISLNK(sa.st_mode)) {
        char x[128], y[128];
        ssize_t nx = readlink(a, x, sizeof x), ny = readlink(b, y, sizeof y);
        return nx == ny && nx >= 0 && !memcmp(x, y, (size_t)nx);
    }
    if ((sa.st_mode & 07777) != (sb.st_mode & 07777)) return 0;
    if (S_ISREG(sa.st_mode)) {
        size_t la, lb;
        unsigned char *x = slurp(a, &la), *y = slurp(b, &lb);
        int r = x && y && la == lb && !memcmp(x, y, la);
        free(x);
        free(y);
        return r;
    }
    int na, nb, r = 1;
    char **va = ls_dir(a, &na), **vb = ls_dir(b, &nb);
    if (na != nb) r = 0;
    for (int i = 0; r && i < na; i++) {
        if (strcmp(va[i], vb[i])) r = 0;
        else {
            char *pa = join2(a, va[i]), *pb = join2(b, vb[i]);
            r = same_tree(pa, pb);
            free(pa);
            free(pb);
        }
    }
    ls_free(va, na);
    ls_free(vb, nb);
    return r;
}

static void report(const char *title) {
    printf("%s: mkdir=%d copy=%d update=%d delete=%d relink=%d chmod=%d replace=%d skipped=%d\n", title,
           counts[A_MKDIR], counts[A_COPY], counts[A_UPDATE], counts[A_DELETE], counts[A_RELINK], counts[A_CHMOD],
           counts[A_REPLACE], counts[A_SKIP]);
    memset(counts, 0, sizeof counts);
}

int main(void) {
    umask(022);
    mkd("src");
    mkd("dst");
    /* source */
    mkd("src/app");
    mkd("src/app/conf");
    mkd("src/data");
    put("src/README", "v2 readme text\n");
    put("src/app/main.c", "int main(void){return 2;}\n");
    put("src/app/conf/site.ini", "[site]\nname=new\n");
    put("src/app/conf/extra.ini", "[extra]\n");
    put("src/data/blob", "0123456789");
    put("src/script.sh", "#!/bin/sh\n");
    CHECK(chmod("src/script.sh", 0755) == 0);
    CHECK(symlink("app/main.c", "src/link") == 0);
    CHECK(symlink("data", "src/datalink") == 0);
    mkd("src/was_file");
    put("src/was_file/inside", "now a dir\n");
    put("src/now_file", "used to be a dir\n");
    mkd("src/newdir");
    mkd("src/newdir/sub");
    put("src/newdir/sub/f", "f");
    CHECK(chmod("src/newdir/sub", 0750) == 0);
    /* destination in an older, partially different state */
    mkd("dst/app");
    mkd("dst/app/conf");
    mkd("dst/data");
    mkd("dst/obsolete");
    put("dst/README", "v1 readme\n");
    put("dst/app/main.c", "int main(void){return 2;}\n"); /* identical */
    put("dst/app/conf/site.ini", "[site]\nname=old\n");
    put("dst/app/conf/stale.ini", "gone\n");
    put("dst/obsolete/file", "x");
    put("dst/data/blob", "0123456789");
    CHECK(chmod("dst/data/blob", 0600) == 0);
    put("dst/script.sh", "#!/bin/sh\n");
    CHECK(symlink("app/other.c", "dst/link") == 0);
    put("dst/was_file", "plain file\n");
    mkd("dst/now_file");
    put("dst/now_file/child", "c");

    printf("dry run:\n");
    dry = 1;
    sync_dir("src", "dst", "");
    report("dry run totals");
    CHECK(!same_tree("src", "dst"));
    CHECK(fsize("dst/README") == 10 && exists_nofollow("dst/obsolete/file"));
    printf("destination untouched by dry run: yes\n");

    printf("first sync:\n");
    dry = 0;
    sync_dir("src", "dst", "");
    report("first sync totals");
    CHECK(same_tree("src", "dst"));
    printf("trees identical after sync: yes\n");

    printf("second sync:\n");
    sync_dir("src", "dst", "");
    report("second sync totals");
    CHECK(same_tree("src", "dst"));

    /* no temp files left behind */
    int n;
    char **v = ls_dir("dst/app/conf", &n);
    for (int i = 0; i < n; i++) CHECK(!strstr(v[i], ".sync-tmp"));
    ls_free(v, n);
    CHECK(rm_rf("src") == 0 && rm_rf("dst") == 0);
    return 0;
}
