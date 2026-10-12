/*
 * title: chmod bits against access() and real open attempts
 * topic: io_files
 * covers: chmod, fchmod, access R_OK W_OK X_OK, stat mode bits, setuid and sticky bits, directory permission semantics, privilege-aware model
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


static int is_root;

/* owner-class model of access(): what a regular non-privileged owner would get */
static int owner_bit(mode_t m, int r, int w, int x) {
    return ((r && (m & 0400)) || (w && (m & 0200)) || (x && (m & 0100))) != 0;
}

/* privileged users bypass r/w checks; x on a regular file needs some x bit */
static int actual_model(mode_t m, int r, int w, int x, int is_dir) {
    if (!is_root) return owner_bit(m, r, w, x);
    if (x) return is_dir || (m & 0111) != 0;
    return 1;
}

static void check_file_modes(void) {
    static const mode_t modes[] = {0000, 0400, 0200, 0100, 0600, 0500, 0300, 0700, 0644, 0755, 0111, 04755};
    printf("file mode -> stat mode | access R W X (owner view)\n");
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        put("t.bin", "#!/bin/true\n");
        CHECK(chmod("t.bin", modes[i]) == 0);
        struct stat st;
        CHECK(stat("t.bin", &st) == 0);
        CHECK((st.st_mode & 07777) == modes[i]);
        int r = access("t.bin", R_OK) == 0, w = access("t.bin", W_OK) == 0, x = access("t.bin", X_OK) == 0;
        CHECK(r == actual_model(modes[i], 1, 0, 0, 0));
        CHECK(w == actual_model(modes[i], 0, 1, 0, 0));
        CHECK(x == actual_model(modes[i], 0, 0, 1, 0));
        printf("%05o -> %05o | %d %d %d\n", (unsigned)modes[i], (unsigned)(st.st_mode & 07777),
               owner_bit(modes[i], 1, 0, 0), owner_bit(modes[i], 0, 1, 0), owner_bit(modes[i], 0, 0, 1));
        CHECK(chmod("t.bin", 0600) == 0);
        CHECK(unlink("t.bin") == 0);
    }
}

static void check_open_attempts(void) {
    printf("open attempts (owner view):\n");
    static const mode_t modes[] = {0000, 0400, 0200, 0600};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        put("o.txt", "x");
        CHECK(chmod("o.txt", modes[i]) == 0);
        errno = 0;
        int fr = open("o.txt", O_RDONLY);
        int er = errno;
        if (fr >= 0) close(fr);
        errno = 0;
        int fw = open("o.txt", O_WRONLY);
        int ew = errno;
        if (fw >= 0) close(fw);
        int want_r = owner_bit(modes[i], 1, 0, 0), want_w = owner_bit(modes[i], 0, 1, 0);
        CHECK((fr >= 0) == (is_root ? 1 : want_r));
        CHECK((fw >= 0) == (is_root ? 1 : want_w));
        if (!is_root) {
            CHECK(fr >= 0 || er == EACCES);
            CHECK(fw >= 0 || ew == EACCES);
        }
        printf("  %04o: read %s, write %s\n", (unsigned)modes[i], want_r ? "ok" : "EACCES",
               want_w ? "ok" : "EACCES");
        CHECK(chmod("o.txt", 0600) == 0);
        CHECK(unlink("o.txt") == 0);
    }
}

static void check_dir_modes(void) {
    printf("directory mode -> list, create, traverse (owner view)\n");
    static const mode_t modes[] = {0700, 0500, 0300, 0100, 0400, 0200, 0000};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        mkd("dm");
        put("dm/inner", "in");
        CHECK(chmod("dm", modes[i]) == 0);
        DIR *d = opendir("dm");
        int can_list = d != NULL;
        if (d) closedir(d);
        int fd = open("dm/new", O_WRONLY | O_CREAT | O_EXCL, 0600);
        int can_create = fd >= 0;
        if (fd >= 0) {
            close(fd);
            CHECK(unlink("dm/new") == 0);
        }
        struct stat st;
        int can_trav = stat("dm/inner", &st) == 0;
        int m = (int)modes[i];
        int want_list = is_root || (m & 0400);
        int want_create = is_root || ((m & 0300) == 0300);
        int want_trav = is_root || (m & 0100);
        CHECK(can_list == (want_list != 0));
        CHECK(can_create == (want_create != 0));
        CHECK(can_trav == (want_trav != 0));
        int o_list = (m & 0400) != 0, o_create = (m & 0300) == 0300, o_trav = (m & 0100) != 0;
        printf("  %04o: list=%d create=%d traverse=%d\n", (unsigned)modes[i], o_list, o_create, o_trav);
        CHECK(rm_rf("dm") == 0);
    }
}

int main(void) {
    umask(022);
    is_root = geteuid() == 0;
    check_file_modes();
    check_open_attempts();
    check_dir_modes();

    /* fchmod, chmod through a symlink, sticky bit on a directory */
    put("target", "t");
    CHECK(symlink("target", "alias") == 0);
    CHECK(chmod("alias", 0640) == 0); /* follows the link */
    struct stat st;
    CHECK(stat("target", &st) == 0);
    printf("chmod via symlink changed target: %04o\n", (unsigned)(st.st_mode & 07777));
    int fd = open("target", O_RDONLY);
    CHECK(fd >= 0 && fchmod(fd, 0444) == 0);
    close(fd);
    CHECK(stat("target", &st) == 0);
    printf("fchmod: %04o\n", (unsigned)(st.st_mode & 07777));
    mkd("shared");
    CHECK(chmod("shared", 01777) == 0);
    CHECK(stat("shared", &st) == 0);
    printf("sticky dir: %05o sticky=%d\n", (unsigned)(st.st_mode & 07777), (st.st_mode & S_ISVTX) != 0);
    /* chmod does not care about umask */
    umask(077);
    CHECK(chmod("shared", 0777) == 0);
    CHECK(stat("shared", &st) == 0);
    printf("chmod under umask 077: %04o\n", (unsigned)(st.st_mode & 0777));
    umask(022);
    errno = 0;
    CHECK(chmod("absent", 0600) < 0);
    printf("chmod(missing): %s\n", en(errno));
    CHECK(rm_rf("target") == 0 && rm_rf("alias") == 0 && rm_rf("shared") == 0);
    return 0;
}
