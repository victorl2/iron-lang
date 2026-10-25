/*
 * title: Driving /bin/cat and /bin/sh through pipes with poll
 * topic: io_files
 * covers: fork, exec, dup2 onto stdin/stdout, nonblocking pipes, poll-driven full duplex transfer without deadlock, exit status, EOF signalling
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

static inline const char *en(int e) {
    switch (e) {
    case 0: return "OK";
    case EEXIST: return "EEXIST";
    case ENOENT: return "ENOENT";
    case EBADF: return "EBADF";
    case EINVAL: return "EINVAL";
    case EISDIR: return "EISDIR";
    case ENOTDIR: return "ENOTDIR";
    case EAGAIN: return "EAGAIN";
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EMFILE: return "EMFILE";
    case ESPIPE: return "ESPIPE";
    case EPIPE: return "EPIPE";
    case EINTR: return "EINTR";
    case EFBIG: return "EFBIG";
    case ENXIO: return "ENXIO";
    case ELOOP: return "ELOOP";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EXDEV: return "EXDEV";
    case ECHILD: return "ECHILD";
    case ESRCH: return "ESRCH";
    default: return "EOTHER";
    }
}

static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned long long fnv(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
#define FNV0 14695981039346656037ULL

static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
typedef struct {
    pid_t pid;
    int to_child;
    int from_child;
} Proc;

static Proc spawn(const char *path, const char *arg0, const char *arg1, const char *arg2) {
    int in[2], out[2];
    CHECK(pipe(in) == 0 && pipe(out) == 0);
    pid_t c = fork();
    CHECK(c >= 0);
    if (c == 0) {
        if (dup2(in[0], 0) != 0 || dup2(out[1], 1) != 1) _exit(120);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        if (arg1) execl(path, arg0, arg1, arg2, (char *)NULL);
        else execl(path, arg0, (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    Proc p = {c, in[1], out[0]};
    return p;
}

/*
 * Send `len` bytes to the child while collecting everything it writes back.
 * Both directions are nonblocking and driven by poll so neither side can stall the other.
 */
static size_t exchange(Proc *p, const unsigned char *src, size_t len, unsigned char *dst, size_t cap) {
    int fl = fcntl(p->to_child, F_GETFL);
    CHECK(fcntl(p->to_child, F_SETFL, fl | O_NONBLOCK) == 0);
    size_t sent = 0, got = 0;
    int in_open = 1, out_open = 1;
    while (out_open) {
        struct pollfd pf[2];
        int n = 0;
        int wi = -1, ri = -1;
        if (in_open) { pf[n].fd = p->to_child; pf[n].events = POLLOUT; pf[n].revents = 0; wi = n++; }
        pf[n].fd = p->from_child; pf[n].events = POLLIN; pf[n].revents = 0; ri = n++;
        int pr = poll(pf, (nfds_t)n, 10000);
        CHECK(pr > 0);
        if (wi >= 0 && (pf[wi].revents & (POLLOUT | POLLERR | POLLHUP))) {
            size_t chunk = len - sent > 1500 ? 1500 : len - sent;
            if (chunk > 0) {
                ssize_t w = write(p->to_child, src + sent, chunk);
                if (w > 0) sent += (size_t)w;
                else CHECK(w < 0 && errno == EAGAIN);
            }
            if (sent == len) { close(p->to_child); in_open = 0; }
        }
        if (pf[ri].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(p->from_child, dst + got, cap - got);
            CHECK(r >= 0);
            if (r == 0) out_open = 0;
            else got += (size_t)r;
        }
    }
    if (in_open) close(p->to_child);
    close(p->from_child);
    return got;
}

int main(void) {
    /* cat echoes 250 KB of pseudo-random bytes; more than pipe capacity in both directions */
    enum { N = 250000 };
    unsigned char *src = malloc(N), *dst = malloc(N + 16);
    CHECK(src && dst);
    for (int i = 0; i < N; i++) src[i] = (unsigned char)(xs() >> 27);
    Proc p = spawn("/bin/cat", "cat", NULL, NULL);
    size_t got = exchange(&p, src, N, dst, N + 16);
    int st = wait_exit(p.pid);
    printf("cat echoed %zu of %d bytes, exit status %d, identical: %d\n", got, N, st, got == N && memcmp(src, dst, N) == 0);
    CHECK(got == N && memcmp(src, dst, N) == 0 && st == 0);
    printf("checksum of echoed data: %016llx\n", fnv(FNV0, dst, got));

    /* empty input: cat exits immediately after EOF */
    p = spawn("/bin/cat", "cat", NULL, NULL);
    got = exchange(&p, src, 0, dst, N);
    st = wait_exit(p.pid);
    printf("cat with empty input: %zu bytes, status %d\n", got, st);
    CHECK(got == 0 && st == 0);

    /* the shell's read builtin consumes lines from our pipe and answers on stdout */
    static const char script[] =
        "read a; read b; read c; echo \"$c-$b-$a\"; exit 3";
    p = spawn("/bin/sh", "sh", "-c", script);
    const char *lines = "one\ntwo\nthree\n";
    got = exchange(&p, (const unsigned char *)lines, strlen(lines), dst, 64);
    st = wait_exit(p.pid);
    dst[got] = 0;
    printf("shell said: %s", (char *)dst);
    printf("shell exit status: %d\n", st);
    CHECK(strcmp((char *)dst, "three-two-one\n") == 0 && st == 3);

    /* /bin/echo needs no input at all */
    p = spawn("/bin/echo", "echo", "spawned", "echo");
    got = exchange(&p, src, 0, dst, 64);
    st = wait_exit(p.pid);
    dst[got] = 0;
    printf("echo said: %s", (char *)dst);
    CHECK(strcmp((char *)dst, "spawned echo\n") == 0 && st == 0);

    /* a shell that never reads stdin: our writes must not hang, the child ends first */
    signal(SIGPIPE, SIG_IGN);
    p = spawn("/bin/sh", "sh", "-c", "echo done; exit 0");
    int fl = fcntl(p.to_child, F_GETFL);
    CHECK(fcntl(p.to_child, F_SETFL, fl | O_NONBLOCK) == 0);
    char c;
    CHECK(read(p.from_child, &c, 1) == 1);
    st = wait_exit(p.pid);
    errno = 0;
    ssize_t w = write(p.to_child, "late", 4);
    printf("write to a finished child: %ld %s, status %d\n", (long)w, en(errno), st);
    CHECK(w < 0 && errno == EPIPE && st == 0);
    close(p.to_child);
    close(p.from_child);
    free(src);
    free(dst);
    return 0;
}
