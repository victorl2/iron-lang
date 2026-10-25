/*
 * title: inet_pton and inet_ntop round trips
 * topic: networking
 * covers: inet_pton, inet_ntop, IPv4 and IPv6 text forms, rejection of malformed input, ENOSPC, address classification, sockaddr_in construction
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
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 16);
}

static const char *classify(uint32_t a) {
    if ((a >> 24) == 127)
        return "loopback";
    if ((a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8)
        return "private";
    if ((a >> 16) == 0xA9FE)
        return "link-local";
    if ((a >> 28) == 0xE)
        return "multicast";
    if (a == 0)
        return "unspecified";
    if (a == 0xFFFFFFFFu)
        return "broadcast";
    return "public";
}

int main(void) {
    static const char *good4[] = {"127.0.0.1",   "0.0.0.0",     "255.255.255.255", "10.20.30.40", "192.168.1.1",
                                  "172.16.254.3", "169.254.7.7", "224.0.0.251",      "8.8.4.4",     "1.2.3.4"};
    for (size_t i = 0; i < sizeof good4 / sizeof good4[0]; i++) {
        struct in_addr a;
        check(inet_pton(AF_INET, good4[i], &a) == 1, "parse");
        char out[INET_ADDRSTRLEN];
        check(inet_ntop(AF_INET, &a, out, sizeof out) != NULL, "format");
        check(strcmp(out, good4[i]) == 0, "round trip");
        uint32_t host = ntohl(a.s_addr);
        printf("%-16s -> 0x%08x %s\n", out, (unsigned)host, classify(host));
    }

    static const char *bad4[] = {"256.1.1.1", "1.2.3", "1.2.3.4.5", "a.b.c.d", "", "1.2.3.4 ", "1..2.3", "-1.2.3.4", "1.2.3.", " 1.2.3.4"};
    int rejected = 0;
    for (size_t i = 0; i < sizeof bad4 / sizeof bad4[0]; i++) {
        struct in_addr a;
        if (inet_pton(AF_INET, bad4[i], &a) == 0)
            rejected++;
    }
    printf("rejected %d of %zu malformed IPv4 strings\n", rejected, sizeof bad4 / sizeof bad4[0]);
    check(rejected == (int)(sizeof bad4 / sizeof bad4[0]), "all rejected");

    /* random addresses: format then parse gives the same 32 bits, and digits match a manual format */
    uint32_t acc = 0;
    for (int i = 0; i < 2000; i++) {
        uint32_t lo = rnd();
        uint32_t hi = rnd();
        uint32_t host = lo ^ (hi << 16);
        struct in_addr a;
        a.s_addr = htonl(host);
        char out[INET_ADDRSTRLEN];
        check(inet_ntop(AF_INET, &a, out, sizeof out) != NULL, "ntop");
        char manual[20];
        snprintf(manual, sizeof manual, "%u.%u.%u.%u", (unsigned)(host >> 24), (unsigned)((host >> 16) & 255),
                 (unsigned)((host >> 8) & 255), (unsigned)(host & 255));
        check(strcmp(out, manual) == 0, "manual format");
        struct in_addr b;
        check(inet_pton(AF_INET, out, &b) == 1 && b.s_addr == a.s_addr, "reparse");
        acc = acc * 31u + host;
    }
    printf("2000 random addresses round-tripped, digest %08x\n", (unsigned)acc);

    /* buffer too small */
    struct in_addr lo;
    lo.s_addr = htonl(INADDR_LOOPBACK);
    char tiny[8];
    errno = 0;
    const char *r = inet_ntop(AF_INET, &lo, tiny, sizeof tiny);
    printf("inet_ntop into 8 bytes: %s\n", r == NULL && errno == ENOSPC ? "ENOSPC" : "unexpected");
    check(r == NULL && errno == ENOSPC, "ENOSPC");

    /* IPv6 text forms are parsed and canonicalised (no sockets are created) */
    static const char *v6[][2] = {{"::1", "::1"},
                                  {"::", "::"},
                                  {"2001:db8::ff00:42:8329", "2001:db8::ff00:42:8329"},
                                  {"2001:0db8:0000:0000:0000:ff00:0042:8329", "2001:db8::ff00:42:8329"},
                                  {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
                                  {"fe80::1", "fe80::1"},
                                  {"FE80:0:0:0:0:0:0:ABCD", "fe80::abcd"},
                                  {"::ffff:1.2.3.4", "::ffff:1.2.3.4"}};
    for (size_t i = 0; i < sizeof v6 / sizeof v6[0]; i++) {
        struct in6_addr a;
        check(inet_pton(AF_INET6, v6[i][0], &a) == 1, "parse v6");
        char out[INET6_ADDRSTRLEN];
        check(inet_ntop(AF_INET6, &a, out, sizeof out) != NULL, "format v6");
        printf("%-42s -> %s\n", v6[i][0], out);
        check(strcmp(out, v6[i][1]) == 0, "canonical v6");
    }
    static const char *bad6[] = {":::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "g::1", "1::2::3", "12345::1"};
    int rej6 = 0;
    for (size_t i = 0; i < sizeof bad6 / sizeof bad6[0]; i++) {
        struct in6_addr a;
        if (inet_pton(AF_INET6, bad6[i], &a) == 0)
            rej6++;
    }
    printf("rejected %d of %zu malformed IPv6 strings\n", rej6, sizeof bad6 / sizeof bad6[0]);
    check(rej6 == (int)(sizeof bad6 / sizeof bad6[0]), "v6 rejected");

    /* build a sockaddr_in from text and read the pieces back */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8080);
    check(inet_pton(AF_INET, "192.0.2.33", &sa.sin_addr) == 1, "sockaddr addr");
    unsigned char raw[4];
    memcpy(raw, &sa.sin_addr, 4);
    printf("sockaddr_in raw address bytes: %u %u %u %u, port %u\n", raw[0], raw[1], raw[2], raw[3],
           (unsigned)ntohs(sa.sin_port));
    return 0;
}
