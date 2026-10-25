/*
 * title: Stop-and-wait reliability over lossy UDP
 * topic: networking
 * covers: sequence numbers, ACK, retransmission on timeout, duplicate suppression, deterministic drop schedule, sendto recvfrom poll
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
/* socket bound to 127.0.0.1:0; stream sockets also listen */
static int make_socket(int type, int backlog) {
    int fd = socket(AF_INET, type, 0);
    check(fd >= 0, "socket");
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    check(bind(fd, (struct sockaddr *)&a, sizeof a) == 0, "bind");
    if (type == SOCK_STREAM)
        check(listen(fd, backlog) == 0, "listen");
    return fd;
}
static struct sockaddr_in local_addr(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    check(getsockname(fd, (struct sockaddr *)&a, &l) == 0, "getsockname");
    check(l == sizeof a && a.sin_family == AF_INET, "sockname family");
    return a;
}

enum { NPKT = 10 };

typedef struct {
    int sock;
    struct sockaddr_in peer;
    int expected;              /* next in-order sequence number */
    int drop_data_once[NPKT];  /* drop first transmission of data seq */
    int drop_ack_once[NPKT];   /* drop first ack for seq */
    int seen_data[NPKT];
    int seen_ack[NPKT];
    int delivered[NPKT];
    int duplicates;
    int data_dropped;
    int acks_dropped;
    char payload[NPKT][32];
} Receiver;

/* the receiver processes the datagram just sent; returns 1 when it sent an ack, 0 when the network dropped something */
static int receiver_step(Receiver *r) {
    check(wait_fd(r->sock, POLLIN, 2000), "data arrives");
    unsigned char pkt[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(r->sock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
    check(n >= 2, "data packet size");
    int seq = pkt[0];
    check(seq >= 0 && seq < NPKT, "seq range");
    r->seen_data[seq]++;
    if (r->drop_data_once[seq] && r->seen_data[seq] == 1) {
        r->data_dropped++;
        return 0;
    }
    if (seq == r->expected) {
        memcpy(r->payload[seq], pkt + 1, (size_t)n - 1);
        r->payload[seq][n - 1] = 0;
        r->delivered[seq] = 1;
        r->expected++;
    } else {
        r->duplicates++;
    }
    r->seen_ack[seq]++;
    if (r->drop_ack_once[seq] && r->seen_ack[seq] == 1) {
        r->acks_dropped++;
        return 0;
    }
    unsigned char ack[2] = {(unsigned char)seq, 0xAC};
    check(sendto(r->sock, ack, 2, 0, (struct sockaddr *)&from, fl) == 2, "send ack");
    return 1;
}

int main(void) {
    int rx = make_socket(SOCK_DGRAM, 0);
    int tx = make_socket(SOCK_DGRAM, 0);
    set_timeout(rx, 2000);
    set_timeout(tx, 2000);
    Receiver r;
    memset(&r, 0, sizeof r);
    r.sock = rx;
    r.peer = local_addr(tx);
    struct sockaddr_in dst = local_addr(rx);
    r.drop_data_once[2] = 1;
    r.drop_data_once[6] = 1;
    r.drop_ack_once[4] = 1;
    r.drop_ack_once[8] = 1;
    r.drop_ack_once[9] = 1;

    static const char *text[NPKT] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    int transmissions[NPKT];
    int total_tx = 0;
    for (int seq = 0; seq < NPKT; seq++) {
        transmissions[seq] = 0;
        int acked = 0;
        while (!acked) {
            check(transmissions[seq] < 5, "retry limit");
            unsigned char pkt[40];
            pkt[0] = (unsigned char)seq;
            size_t tl = strlen(text[seq]);
            memcpy(pkt + 1, text[seq], tl);
            check(sendto(tx, pkt, tl + 1, 0, (struct sockaddr *)&dst, sizeof dst) == (ssize_t)(tl + 1), "send data");
            transmissions[seq]++;
            total_tx++;
            /* a lost packet or ack is a retransmission timeout, simulated without real waiting */
            int ack_sent = receiver_step(&r);
            if (ack_sent && wait_fd(tx, POLLIN, 2000)) {
                unsigned char ack[4];
                ssize_t n = recv(tx, ack, sizeof ack, 0);
                check(n == 2 && ack[1] == 0xAC, "ack format");
                if (ack[0] == seq)
                    acked = 1;
            }
        }
    }
    for (int i = 0; i < NPKT; i++)
        printf("seq %d '%s': %d transmission%s\n", i, r.payload[i], transmissions[i], transmissions[i] == 1 ? "" : "s");
    printf("total transmissions %d, data dropped %d, acks dropped %d, duplicates suppressed %d\n", total_tx,
           r.data_dropped, r.acks_dropped, r.duplicates);
    check(r.expected == NPKT, "all delivered in order");
    check(total_tx == NPKT + r.data_dropped + r.acks_dropped, "retransmission arithmetic");
    for (int i = 0; i < NPKT; i++)
        check(strcmp(r.payload[i], text[i]) == 0, "payload intact");
    printf("delivered in order without gaps: yes\n");
    close(rx);
    close(tx);
    return 0;
}
