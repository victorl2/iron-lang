/*
 * title: make-style rebuild decisions driven by file mtimes set with utimes
 * topic: io_files
 * covers: utimes with a logical clock, st_mtime comparison, dependency graph DFS, missing target vs stale target, equal-mtime not rebuilt, missing prerequisite error, cycle detection, incremental rebuild scenarios
 * deps: libc, posix
 */
#include <sys/time.h>
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


typedef struct {
    const char *target;
    const char *deps[4];
} Rule;

static const Rule rules[] = {
    {"all", {"app", NULL}},
    {"app", {"main.o", "util.o", "lib.a", NULL}},
    {"main.o", {"main.c", "common.h", NULL}},
    {"util.o", {"util.c", "common.h", NULL}},
    {"lib.a", {"util.o", "extra.o", NULL}},
    {"extra.o", {"extra.c", NULL}},
    {"cyc1", {"cyc2", NULL}},
    {"cyc2", {"cyc1", NULL}},
    {"broken", {"nothere.c", NULL}},
};

static long clock_now = 1700000000L;
static int built, state[16]; /* 0 unvisited, 1 in progress, 2 done */

static void set_mtime(const char *path, long t) {
    struct timeval tv[2];
    tv[0].tv_sec = t;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    CHECK(utimes(path, tv) == 0);
}

static long mtime_of(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    return (long)st.st_mtime;
}

static void touch(const char *path) {
    if (!exists_nofollow(path)) put(path, "src");
    clock_now += 10;
    set_mtime(path, clock_now);
}

static int rule_index(const char *t) {
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++)
        if (!strcmp(rules[i].target, t)) return (int)i;
    return -1;
}

/* returns 0 ok, -1 error */
static int make(const char *t, int indent) {
    int ri = rule_index(t);
    if (ri < 0) {
        if (exists_nofollow(t)) return 0;
        printf("%*sno rule to make target '%s'\n", indent, "", t);
        return -1;
    }
    if (state[ri] == 2) return 0;
    if (state[ri] == 1) {
        printf("%*scircular dependency detected at '%s'\n", indent, "", t);
        return -1;
    }
    state[ri] = 1;
    long tm = mtime_of(t);
    int newest_dep_newer = 0;
    const char *why_dep = NULL;
    for (int i = 0; rules[ri].deps[i]; i++) {
        const char *d = rules[ri].deps[i];
        if (make(d, indent + 2) < 0) return -1;
        long dm = mtime_of(d);
        if (tm >= 0 && dm > tm && !newest_dep_newer) {
            newest_dep_newer = 1;
            why_dep = d;
        }
    }
    if (!strcmp(t, "all")) { /* phony goal: only its prerequisites matter */
        state[ri] = 2;
        return 0;
    }
    if (tm < 0 || newest_dep_newer) {
        /* "compile": content records the number of dependencies and a build counter */
        char buf[64];
        snprintf(buf, sizeof buf, "built #%d", ++built);
        put(t, buf);
        clock_now += 10;
        set_mtime(t, clock_now);
        if (tm < 0) printf("%*sbuild %-8s (missing)\n", indent, "", t);
        else printf("%*sbuild %-8s (older than %s)\n", indent, "", t, why_dep);
    }
    state[ri] = 2;
    return 0;
}

static void run(const char *label, const char *goal) {
    memset(state, 0, sizeof state);
    int before = built;
    printf("== %s: make %s\n", label, goal);
    int rc = make(goal, 2);
    printf("   -> %s, %d rebuilt\n", rc == 0 ? "ok" : "failed", built - before);
}

int main(void) {
    umask(022);
    static const char *srcs[] = {"main.c", "util.c", "extra.c", "common.h"};
    for (int i = 0; i < 4; i++) touch(srcs[i]);
    printf("logical mtimes of sources: %ld %ld %ld %ld\n", mtime_of("main.c") - 1700000000L,
           mtime_of("util.c") - 1700000000L, mtime_of("extra.c") - 1700000000L, mtime_of("common.h") - 1700000000L);

    run("clean tree", "all");
    run("nothing changed", "all");

    touch("common.h");
    run("common.h touched", "all");

    touch("util.c");
    run("util.c touched", "all");

    CHECK(unlink("main.o") == 0);
    run("main.o deleted", "all");

    /* equal mtimes: a target with the same mtime as its dependency is considered current */
    long t = mtime_of("extra.o");
    set_mtime("extra.c", t);
    run("extra.c mtime == extra.o mtime", "all");
    set_mtime("extra.c", t + 1);
    run("extra.c one second newer", "all");

    /* an old source (older than every object) never triggers work */
    set_mtime("common.h", 1600000000L);
    run("common.h made ancient", "all");
    printf("common.h mtime is now %ld (epoch seconds, set by us)\n", mtime_of("common.h"));

    run("cycle", "cyc1");
    run("missing prerequisite", "broken");
    printf("total build steps: %d\n", built);
    CHECK(built > 0);

    static const char *all[] = {"main.c", "util.c", "extra.c", "common.h", "main.o", "util.o", "extra.o", "lib.a", "app"};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) CHECK(rm_rf(all[i]) == 0);
    return 0;
}
