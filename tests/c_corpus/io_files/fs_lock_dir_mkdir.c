/*
 * title: Lock directories via mkdir with contending processes and stale lock recovery
 * topic: io_files
 * covers: mkdir as an atomic test-and-set, EEXIST as lock-busy, owner file inside the lock, contending children incrementing a shared counter, stale lock detection via kill(pid, 0), lock stealing
 * deps: libc, posix
 */
#include <sched.h>
#include <signal.h>
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


#define LOCK "counter.lock"
#define WORKERS 4
#define ROUNDS 60

static void lock_acquire(void) {
    for (;;) {
        if (mkdir(LOCK, 0700) == 0) return;
        CHECK(errno == EEXIST);
        sched_yield();
    }
}

static void lock_release(void) { CHECK(rmdir(LOCK) == 0); }

static long read_counter(void) {
    size_t n;
    unsigned char *b = slurp("counter", &n);
    CHECK(b);
    long v = strtol((char *)b, NULL, 10);
    free(b);
    return v;
}

static void worker(int id) {
    for (int i = 0; i < ROUNDS; i++) {
        lock_acquire();
        /* deliberately slow read-modify-write: without the lock updates would be lost */
        long v = read_counter();
        char buf[32];
        snprintf(buf, sizeof buf, "%ld", v + 1);
        put("counter", buf);
        lock_release();
    }
    (void)id;
    _exit(0);
}

/* returns 1 and takes the lock if it was free or the owner is dead */
static int lock_try_steal(void) {
    if (mkdir(LOCK, 0700) == 0) return 1;
    CHECK(errno == EEXIST);
    size_t n;
    unsigned char *b = slurp(LOCK "/owner", &n);
    if (!b) return 0; /* lock exists but no owner file yet: assume being set up */
    long pid = strtol((char *)b, NULL, 10);
    free(b);
    if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) return 0;
    /* stale: owner is gone; break it */
    CHECK(unlink(LOCK "/owner") == 0 && rmdir(LOCK) == 0);
    return mkdir(LOCK, 0700) == 0;
}

int main(void) {
    umask(022);
    put("counter", "0");

    /* basic semantics */
    CHECK(mkdir(LOCK, 0700) == 0);
    errno = 0;
    int rc = mkdir(LOCK, 0700);
    printf("second mkdir on held lock: %s\n", rc == 0 ? "OK" : en(errno));
    CHECK(rc < 0 && errno == EEXIST);
    CHECK(rmdir(LOCK) == 0);
    printf("lock released, free again: %s\n", mkdir(LOCK, 0700) == 0 ? "yes" : "no");
    CHECK(rmdir(LOCK) == 0);

    /* WORKERS processes hammer one counter */
    pid_t kids[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        kids[i] = fork();
        CHECK(kids[i] >= 0);
        if (kids[i] == 0) worker(i);
    }
    int ok = 0;
    for (int i = 0; i < WORKERS; i++) {
        int st;
        CHECK(waitpid(kids[i], &st, 0) == kids[i]);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++;
    }
    long total = read_counter();
    printf("workers ok=%d, counter=%ld (expected %d)\n", ok, total, WORKERS * ROUNDS);
    CHECK(ok == WORKERS && total == WORKERS * ROUNDS);
    CHECK(!exists_nofollow(LOCK));

    /* stale lock: a child takes the lock, records its pid, and dies without releasing */
    pid_t dead = fork();
    CHECK(dead >= 0);
    if (dead == 0) {
        if (mkdir(LOCK, 0700) != 0) _exit(2);
        char buf[32];
        snprintf(buf, sizeof buf, "%ld\n", (long)getpid());
        if (put_bytes(LOCK "/owner", buf, strlen(buf)) != 0) _exit(3);
        _exit(0);
    }
    int st;
    CHECK(waitpid(dead, &st, 0) == dead && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("child died holding the lock: lock exists=%d\n", exists_nofollow(LOCK));
    CHECK(exists_nofollow(LOCK));
    int stolen = lock_try_steal();
    printf("stale lock detected and stolen: %s\n", stolen ? "yes" : "no");
    CHECK(stolen);

    /* a live owner cannot be robbed: record our own pid */
    char me[32];
    snprintf(me, sizeof me, "%ld\n", (long)getpid());
    put(LOCK "/owner", me);
    int robbed = lock_try_steal();
    printf("lock held by a live process stolen: %s\n", robbed ? "yes" : "no");
    CHECK(!robbed);
    CHECK(unlink(LOCK "/owner") == 0 && rmdir(LOCK) == 0);
    CHECK(rm_rf("counter") == 0);
    return 0;
}
