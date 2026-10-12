/*
 * title: Numbered backup rotation (app.log.1 ... app.log.N) with gaps and copytruncate
 * topic: io_files
 * covers: rename chain shifting from oldest to newest, unlinking the oldest, tolerating missing generations, open fd follows renamed inode, copytruncate alternative, generation bookkeeping
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


#define KEEP 4

static void gen_name(char *out, size_t cap, int i) {
    if (i == 0) snprintf(out, cap, "app.log");
    else snprintf(out, cap, "app.log.%d", i);
}

/* shift app.log.(k-1) -> app.log.k for k = KEEP..2, drop app.log.KEEP first; missing entries are skipped */
static int rotate(void) {
    char from[32], to[32];
    gen_name(to, sizeof to, KEEP);
    if (unlink(to) < 0 && errno != ENOENT) return -1;
    for (int k = KEEP; k >= 1; k--) {
        gen_name(from, sizeof from, k - 1);
        gen_name(to, sizeof to, k);
        if (rename(from, to) < 0 && errno != ENOENT) return -1;
    }
    return 0;
}

static void show_state(void) {
    int n;
    char **v = ls_dir(".", &n);
    for (int i = 0; i < n; i++) {
        if (strncmp(v[i], "app.log", 7) != 0) continue;
        size_t len;
        unsigned char *b = slurp(v[i], &len);
        CHECK(b);
        while (len && (b[len - 1] == '\n')) b[--len] = 0;
        printf("  %-10s [%s]\n", v[i], b);
        free(b);
    }
    ls_free(v, n);
}

int main(void) {
    umask(022);
    /* 7 generations: after each rotation app.log.i holds generation (gen - i) */
    for (int gen = 1; gen <= 7; gen++) {
        char line[32];
        snprintf(line, sizeof line, "generation %d\n", gen);
        if (gen > 1) CHECK(rotate() == 0);
        put("app.log", line);
        for (int i = 1; i <= KEEP; i++) {
            char nm[32];
            gen_name(nm, sizeof nm, i);
            int want = gen - i;
            if (want < 1) {
                CHECK(!exists_nofollow(nm));
                continue;
            }
            size_t len;
            unsigned char *b = slurp(nm, &len);
            CHECK(b);
            char exp[32];
            snprintf(exp, sizeof exp, "generation %d\n", want);
            CHECK(strcmp((char *)b, exp) == 0);
            free(b);
        }
        if (gen == 3 || gen == 7) {
            printf("after generation %d:\n", gen);
            show_state();
        }
    }

    /* gaps: remove .2, rotate: existing entries keep their relative order, nothing is overwritten wrongly */
    CHECK(unlink("app.log.2") == 0);
    CHECK(rotate() == 0);
    put("app.log", "generation 8\n");
    printf("after removing .2 and rotating (gen 8):\n");
    show_state();
    /* .1 was gen 6 -> now .2; gap moved: .1 is the previous current (gen 7) */
    size_t len;
    unsigned char *b = slurp("app.log.1", &len);
    CHECK(b && strcmp((char *)b, "generation 7\n") == 0);
    free(b);
    b = slurp("app.log.2", &len);
    CHECK(b && strcmp((char *)b, "generation 6\n") == 0);
    free(b);

    /* rename-style rotation: a writer holding the fd keeps writing into the rotated file */
    CHECK(rm_rf("app.log.1") == 0 && rm_rf("app.log.2") == 0 && rm_rf("app.log.3") == 0 && rm_rf("app.log.4") == 0);
    int wfd = open("app.log", O_WRONLY | O_APPEND);
    CHECK(wfd >= 0);
    CHECK(rotate() == 0);
    CHECK(write(wfd, "late line\n", 10) == 10);
    printf("rename rotation, writer still open:\n");
    printf("  app.log exists=%d, app.log.1 has %ld bytes\n", exists_nofollow("app.log"), fsize("app.log.1"));
    CHECK(!exists_nofollow("app.log") && fsize("app.log.1") == 23);
    close(wfd);

    /* copytruncate rotation: copy content aside, truncate in place, the writer keeps its fd valid */
    CHECK(rm_rf("app.log.1") == 0);
    put("app.log", "current data\n");
    wfd = open("app.log", O_WRONLY | O_APPEND);
    CHECK(wfd >= 0);
    size_t n;
    unsigned char *cur = slurp("app.log", &n);
    CHECK(put_bytes("app.log.copy", cur, n) == 0);
    free(cur);
    CHECK(truncate("app.log", 0) == 0);
    CHECK(write(wfd, "after\n", 6) == 6); /* O_APPEND: lands at the new end, offset 0 */
    close(wfd);
    printf("copytruncate: app.log=%ld bytes, app.log.copy=%ld bytes\n", fsize("app.log"), fsize("app.log.copy"));
    CHECK(fsize("app.log") == 6 && fsize("app.log.copy") == 13);

    int cnt;
    char **v = ls_dir(".", &cnt);
    for (int i = 0; i < cnt; i++) CHECK(rm_rf(v[i]) == 0);
    ls_free(v, cnt);
    return 0;
}
