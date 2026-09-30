/*
 * title: umask filtering of open and mkdir modes
 * topic: io_files
 * covers: umask, open mode argument, mkdir mode, fchmod bypassing umask, existing files keep their mode, fork inherits umask
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);          \
            exit(1);                                                      \
        }                                                                 \
    } while (0)



static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
static unsigned mode_of(const char *path) {
    struct stat st;
    CHECK(stat(path, &st) == 0);
    return (unsigned)(st.st_mode & 0777);
}

int main(void) {
    mode_t old = umask(022);
    (void)old;

    /* creation mode is requested & ~umask */
    static const mode_t masks[] = {022, 077, 0, 027, 007, 0777};
    static const mode_t reqs[] = {0666, 0644, 0777, 0600, 0755};
    int checked = 0;
    for (unsigned m = 0; m < sizeof masks / sizeof masks[0]; m++) {
        umask(masks[m]);
        for (unsigned r = 0; r < sizeof reqs / sizeof reqs[0]; r++) {
            int fd = open("probe", O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, reqs[r]);
            CHECK(fd >= 0);
            close(fd);
            unsigned got = mode_of("probe");
            unsigned want = (unsigned)(reqs[r] & ~masks[m] & 0777);
            CHECK(got == want);
            checked++;
            CHECK(unlink("probe") == 0);
        }
        printf("umask %03o: open(0666)->%03o open(0755)->%03o\n", (unsigned)masks[m],
               (unsigned)(0666 & ~masks[m] & 0777), (unsigned)(0755 & ~masks[m] & 0777));
    }
    printf("open modes verified: %d\n", checked);

    /* mkdir applies the umask the same way */
    umask(027);
    CHECK(mkdir("d1", 0777) == 0);
    printf("mkdir 0777 under umask 027: %03o\n", mode_of("d1"));
    CHECK(mode_of("d1") == 0750);
    umask(0);
    CHECK(mkdir("d2", 0777) == 0);
    printf("mkdir 0777 under umask 000: %03o\n", mode_of("d2"));
    CHECK(mode_of("d2") == 0777);

    /* fchmod and chmod ignore the umask */
    umask(077);
    int fd = open("f", O_RDWR | O_CREAT | O_TRUNC, 0666);
    CHECK(fd >= 0);
    printf("created under umask 077: %03o\n", mode_of("f"));
    CHECK(fchmod(fd, 0666) == 0);
    printf("after fchmod 0666: %03o\n", mode_of("f"));
    CHECK(mode_of("f") == 0666);
    CHECK(chmod("f", 0755) == 0);
    CHECK(mode_of("f") == 0755);
    close(fd);

    /* opening an existing file with O_CREAT does not touch its mode */
    umask(022);
    fd = open("f", O_RDWR | O_CREAT, 0600);
    CHECK(fd >= 0);
    printf("existing file reopened with O_CREAT 0600: %03o\n", mode_of("f"));
    CHECK(mode_of("f") == 0755);
    close(fd);

    /* umask returns the previous value and children inherit it without affecting the parent */
    mode_t prev = umask(0123);
    printf("umask() returned the previous mask: %03o\n", (unsigned)prev);
    CHECK(prev == 022);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        mode_t inherited = umask(0);
        _exit(inherited == 0123 ? 0 : 1);
    }
    CHECK(wait_exit(c) == 0);
    mode_t still = umask(022);
    printf("parent mask after child changed its own: %03o\n", (unsigned)still);
    CHECK(still == 0123);

    /* exec keeps the mask too: create a file from the shell and inspect it */
    umask(027);
    c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        execl("/bin/sh", "sh", "-c", ": > from_sh", (char *)NULL);
        _exit(127);
    }
    CHECK(wait_exit(c) == 0);
    printf("file created by /bin/sh under umask 027: %03o\n", mode_of("from_sh"));
    CHECK(mode_of("from_sh") == 0640);

    unlink("from_sh");
    unlink("f");
    rmdir("d1");
    rmdir("d2");
    return 0;
}
