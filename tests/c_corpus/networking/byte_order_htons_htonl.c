/*
 * title: Byte order conversion and the Internet checksum
 * topic: networking
 * covers: htons, htonl, ntohs, ntohl, manual big-endian codecs, 64-bit swap, memory layout, RFC 1071 checksum with known answer
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

static void put_be16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}
static void put_be32(unsigned char *p, uint32_t v) {
    put_be16(p, (uint16_t)(v >> 16));
    put_be16(p + 2, (uint16_t)v);
}
static void put_be64(unsigned char *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}
static uint16_t get_be16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_be32(const unsigned char *p) { return ((uint32_t)get_be16(p) << 16) | get_be16(p + 2); }
static uint64_t get_be64(const unsigned char *p) { return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4); }

static uint64_t swap64(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        r = (r << 8) | (v & 0xff);
        v >>= 8;
    }
    return r;
}

/* RFC 1071: one's complement sum of 16-bit words */
static uint16_t inet_checksum(const unsigned char *p, size_t n) {
    uint32_t sum = 0;
    while (n > 1) {
        sum += get_be16(p);
        p += 2;
        n -= 2;
    }
    if (n)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

int main(void) {
    /* the bytes in memory of a network order value are always the big-endian digits */
    uint16_t n16 = htons(0x1234);
    unsigned char b[8];
    memcpy(b, &n16, 2);
    printf("htons(0x1234) bytes: %02x %02x\n", b[0], b[1]);
    check(b[0] == 0x12 && b[1] == 0x34, "htons layout");
    uint32_t n32 = htonl(0xA1B2C3D4u);
    memcpy(b, &n32, 4);
    printf("htonl(0xA1B2C3D4) bytes: %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3]);
    check(b[0] == 0xA1 && b[3] == 0xD4, "htonl layout");
    check(ntohs(n16) == 0x1234 && ntohl(n32) == 0xA1B2C3D4u, "ntoh inverse");

    /* codecs agree with the library on random values */
    uint32_t digest = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t lo = rnd();
        uint32_t hi = rnd();
        uint32_t v = lo ^ (hi << 16);
        unsigned char e[4], l[4];
        put_be32(e, v);
        uint32_t nv = htonl(v);
        memcpy(l, &nv, 4);
        check(memcmp(e, l, 4) == 0, "put_be32 equals htonl");
        check(get_be32(l) == v && ntohl(nv) == v, "get_be32");
        uint16_t s = (uint16_t)(v >> 7);
        unsigned char e2[2], l2[2];
        put_be16(e2, s);
        uint16_t ns = htons(s);
        memcpy(l2, &ns, 2);
        check(memcmp(e2, l2, 2) == 0 && get_be16(l2) == s, "16-bit codec");
        digest = digest * 33u + v;
    }
    printf("1000 random values verified, digest %08x\n", (unsigned)digest);

    uint64_t big = 0x0102030405060708ull;
    put_be64(b, big);
    printf("be64 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    check(get_be64(b) == big, "be64 round trip");
    printf("swap64 -> %016llx\n", (unsigned long long)swap64(big));
    check(swap64(swap64(big)) == big && swap64(big) == 0x0807060504030201ull, "swap64");

    /* an IPv4 header whose checksum is the classic worked example */
    unsigned char hdr[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                             0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    uint16_t ck = inet_checksum(hdr, sizeof hdr);
    printf("ipv4 header checksum: 0x%04x\n", ck);
    check(ck == 0xb861, "known checksum");
    put_be16(hdr + 10, ck);
    check(inet_checksum(hdr, sizeof hdr) == 0, "verifies to zero");
    printf("checksum of header including its checksum: 0x%04x\n", inet_checksum(hdr, sizeof hdr));
    /* corrupting one bit is detected */
    hdr[15] ^= 0x10;
    uint16_t bad = inet_checksum(hdr, sizeof hdr);
    check(bad != 0, "corruption detected");
    printf("after a bit flip the check is nonzero: yes\n");
    /* odd length payloads are padded with a zero byte */
    unsigned char odd[5] = {0x01, 0x02, 0x03, 0x04, 0x05};
    unsigned char padded[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x00};
    check(inet_checksum(odd, 5) == inet_checksum(padded, 6), "odd padding");
    printf("odd length checksum: 0x%04x\n", inet_checksum(odd, 5));
    return 0;
}
