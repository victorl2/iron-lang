/*
 * title: Passing file descriptors over a unix socketpair
 * topic: networking
 * covers: sendmsg, recvmsg, SCM_RIGHTS, CMSG macros, pipe and socket descriptors passed, several fds in one message, fd duplication semantics
 * deps: libc, posix, sockets
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}
static void set_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    check(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0, "SO_RCVTIMEO");
    check(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) == 0, "SO_SNDTIMEO");
}
static int wait_fd(int fd, short ev, int ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = ev;
    p.revents = 0;
    int r;
    do {
        r = poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}
static int send_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
/* reads until n bytes, EOF, or error; returns bytes read */
static size_t recv_all(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    return got;
}

typedef union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(4 * sizeof(int))];
} CtlBuf;

/* sends a one byte payload and nfds descriptors */
static void send_fds(int sock, const int *fds, int nfds, char tag) {
    struct msghdr msg;
    struct iovec iov;
    CtlBuf ctl;
    memset(&msg, 0, sizeof msg);
    memset(&ctl, 0, sizeof ctl);
    iov.iov_base = &tag;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = CMSG_SPACE((size_t)nfds * sizeof(int));
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN((size_t)nfds * sizeof(int));
    memcpy(CMSG_DATA(cm), fds, (size_t)nfds * sizeof(int));
    check(sendmsg(sock, &msg, 0) == 1, "sendmsg");
}

/* receives up to cap descriptors, returns how many; stores the tag byte */
static int recv_fds(int sock, int *fds, int cap, char *tag) {
    struct msghdr msg;
    struct iovec iov;
    CtlBuf ctl;
    memset(&msg, 0, sizeof msg);
    memset(&ctl, 0, sizeof ctl);
    iov.iov_base = tag;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof ctl.buf;
    check(wait_fd(sock, POLLIN, 2000), "fd message readable");
    check(recvmsg(sock, &msg, 0) == 1, "recvmsg");
    int n = 0;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
            continue;
        int count = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        check(n + count <= cap, "fd capacity");
        memcpy(fds + n, CMSG_DATA(cm), (size_t)count * sizeof(int));
        n += count;
    }
    return n;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int ch[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, ch) == 0, "control socketpair");
    set_timeout(ch[0], 2000);
    set_timeout(ch[1], 2000);

    /* 1: pass the read end of a pipe, the receiver reads what the sender writes */
    int pp[2];
    check(pipe(pp) == 0, "pipe");
    send_fds(ch[0], &pp[0], 1, 'P');
    close(pp[0]);
    int got[4];
    char tag = 0;
    int n = recv_fds(ch[1], got, 4, &tag);
    printf("message '%c' carried %d descriptor(s)\n", tag, n);
    check(n == 1 && tag == 'P', "one fd");
    check(write(pp[1], "through the pipe", 16) == 16, "write pipe");
    close(pp[1]);
    char buf[32];
    ssize_t r = read(got[0], buf, sizeof buf);
    check(r == 16, "read via passed fd");
    buf[r] = 0;
    printf("read via the received descriptor: %s\n", buf);
    check(read(got[0], buf, sizeof buf) == 0, "EOF after writer closed");
    close(got[0]);

    /* 2: three descriptors in a single message */
    int pipes[3][2];
    int rd[3];
    for (int i = 0; i < 3; i++) {
        check(pipe(pipes[i]) == 0, "pipe");
        rd[i] = pipes[i][0];
    }
    send_fds(ch[0], rd, 3, 'T');
    for (int i = 0; i < 3; i++)
        close(pipes[i][0]);
    n = recv_fds(ch[1], got, 4, &tag);
    printf("message '%c' carried %d descriptor(s)\n", tag, n);
    check(n == 3 && tag == 'T', "three fds");
    /* the fds arrive in order: write distinct data to each pipe and read through the matching received fd */
    for (int i = 0; i < 3; i++) {
        char m[8];
        int len = snprintf(m, sizeof m, "pipe-%d", i);
        check(write(pipes[i][1], m, (size_t)len) == len, "write");
    }
    for (int i = 0; i < 3; i++) {
        r = read(got[i], buf, sizeof buf);
        buf[r > 0 ? r : 0] = 0;
        printf("descriptor %d yields: %s\n", i, buf);
        char want[8];
        snprintf(want, sizeof want, "pipe-%d", i);
        check(strcmp(buf, want) == 0, "fd order");
        close(got[i]);
        close(pipes[i][1]);
    }

    /* 3: pass one end of another socketpair: the receiver talks to the original peer */
    int inner[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, inner) == 0, "inner socketpair");
    send_fds(ch[0], &inner[1], 1, 'S');
    close(inner[1]);
    n = recv_fds(ch[1], got, 4, &tag);
    check(n == 1 && tag == 'S', "socket fd");
    set_timeout(inner[0], 2000);
    set_timeout(got[0], 2000);
    check(send_all(inner[0], "question?", 9) == 0, "send question");
    char q[16];
    check(recv_all(got[0], q, 9) == 9 && memcmp(q, "question?", 9) == 0, "receiver hears question");
    check(send_all(got[0], "answer!", 7) == 0, "send answer");
    char a[16];
    check(recv_all(inner[0], a, 7) == 7 && memcmp(a, "answer!", 7) == 0, "sender hears answer");
    printf("a passed socket still talks to its original peer: yes\n");
    close(got[0]);
    /* closing the last copy of the passed end gives the peer EOF */
    char eof;
    check(recv(inner[0], &eof, 1, 0) == 0, "EOF after last close");
    printf("peer sees EOF once all copies are closed: yes\n");
    close(inner[0]);

    /* 4: a regular file descriptor keeps its offset shared with the sender's copy */
    char path[] = "fdpass_XXXXXX";
    int f = mkstemp(path);
    check(f >= 0, "mkstemp");
    check(write(f, "0123456789", 10) == 10, "write file");
    check(lseek(f, 0, SEEK_SET) == 0, "rewind");
    send_fds(ch[0], &f, 1, 'F');
    n = recv_fds(ch[1], got, 4, &tag);
    check(n == 1, "file fd");
    char two[3];
    check(read(got[0], two, 2) == 2, "read via received fd");
    two[2] = 0;
    char three[4];
    check(read(f, three, 3) == 3, "read via original fd");
    three[3] = 0;
    printf("received fd read '%s', original fd continued with '%s'\n", two, three);
    check(strcmp(two, "01") == 0 && strcmp(three, "234") == 0, "shared offset");
    close(got[0]);
    close(f);
    unlink(path);
    close(ch[0]);
    close(ch[1]);
    return 0;
}
