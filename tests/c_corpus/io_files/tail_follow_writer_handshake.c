/*
 * title: tail -f style follower driven by a writer process with pipe handshakes
 * topic: io_files
 * covers: follow mode, EOF polling without sleeping, partial line buffering, truncation detection, wake/ack pipe protocol, fork
 * deps: libc, posix
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static inline uint64_t rnd(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t rndn(uint32_t n) {
    uint64_t v = rnd();
    return (uint32_t)((v >> 16) % n);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint32_t crc32_update(uint32_t crc, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static inline uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read up to n bytes at offset; returns the number of bytes read (short at EOF). */
static inline size_t pread_upto(int fd, void *buf, size_t n, off_t off) {
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
        check(r >= 0, "pread");
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got;
}

static inline void write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = (const unsigned char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        check(w > 0, "write");
        p += w;
        n -= (size_t)w;
    }
}

static inline long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Read a whole file into a malloc'd buffer (caller frees). */
static inline unsigned char *slurp(const char *path, size_t *len) {
    long n = file_size(path);
    check(n >= 0, "slurp stat");
    unsigned char *b = (unsigned char *)malloc((size_t)n + 1);
    check(b != NULL, "malloc");
    int fd = open(path, O_RDONLY);
    check(fd >= 0, "slurp open");
    size_t got = pread_upto(fd, b, (size_t)n, 0);
    close(fd);
    check(got == (size_t)n, "slurp short");
    *len = (size_t)n;
    return b;
}

static inline void spit(const char *path, const void *buf, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "spit open");
    write_all(fd, buf, n);
    close(fd);
}

#include <signal.h>
#include <sys/wait.h>

typedef struct {
    long lines;
    long truncations;
    uint32_t crc;
} Totals;

/* Follower: after each wake byte, read everything new from the log file, emit complete lines only. */
static void follower(int wake_fd, int ack_fd) {
    alarm(20);
    int fd = open("app.log", O_RDONLY);
    check(fd >= 0, "follower open");
    off_t offset = 0;
    char partial[256];
    size_t plen = 0;
    Totals tot = {0, 0, 0};
    for (;;) {
        unsigned char cmd;
        ssize_t r = read(wake_fd, &cmd, 1);
        if (r <= 0 || cmd == 'Q')
            break;
        struct stat st;
        check(fstat(fd, &st) == 0, "fstat");
        int truncated = 0;
        if (st.st_size < offset) { /* file shrank: log was truncated/rotated in place */
            offset = 0;
            plen = 0;
            tot.truncations++;
            truncated = 1;
        }
        long got_lines = 0;
        unsigned char buf[64];
        size_t n;
        while ((n = pread_upto(fd, buf, sizeof buf, offset)) > 0) {
            offset += (off_t)n;
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == '\n') {
                    tot.crc = crc32_update(tot.crc, partial, plen);
                    tot.lines++;
                    got_lines++;
                    plen = 0;
                } else {
                    check(plen < sizeof partial, "line too long");
                    partial[plen++] = (char)buf[i];
                }
            }
        }
        char ack[64];
        int al = snprintf(ack, sizeof ack, "%ld %zu %d\n", got_lines, plen, truncated);
        write_all(ack_fd, ack, (size_t)al);
    }
    unsigned char rep[16];
    put32(rep, (uint32_t)tot.lines);
    put32(rep + 4, (uint32_t)tot.truncations);
    put32(rep + 8, tot.crc);
    write_all(ack_fd, "END\n", 4);
    write_all(ack_fd, rep, 12);
    close(fd);
    _exit(0);
}

static void read_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        check(r == 1, "ack read");
        if (c == '\n')
            break;
        out[n++] = c;
    }
    out[n] = 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int wake[2], ack[2];
    check(pipe(wake) == 0 && pipe(ack) == 0, "pipes");
    spit("app.log", "", 0);
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        close(wake[1]);
        close(ack[0]);
        follower(wake[0], ack[1]);
    }
    close(wake[0]);
    close(ack[1]);

    int wfd = open("app.log", O_WRONLY | O_APPEND);
    check(wfd >= 0, "writer open");
    uint32_t exp_crc = 0;
    long exp_lines = 0;
    /* Each round: what the writer does, then a wake byte and a wait for the follower's ack. */
    static const char *desc[9] = {"3 full lines",       "nothing new",         "line split across two writes (first half)",
                                  "second half completes the line", "burst of 20 lines", "truncate the file",
                                  "2 lines after truncation", "half line, then rotate-truncate", "final 4 lines"};
    long expect_new[9] = {3, 0, 0, 1, 20, 0, 2, 0, 4};
    for (int round = 0; round < 9; round++) {
        char line[64];
        int ll;
        switch (round) {
        case 0:
        case 4:
        case 6:
        case 8: {
            int n = round == 4 ? 20 : (round == 0 ? 3 : (round == 6 ? 2 : 4));
            for (int i = 0; i < n; i++) {
                ll = snprintf(line, sizeof line, "event r%d #%d val=%u\n", round, i, rndn(1000));
                write_all(wfd, line, (size_t)ll);
                exp_crc = crc32_update(exp_crc, line, (size_t)ll - 1);
                exp_lines++;
            }
            break;
        }
        case 2:
            ll = snprintf(line, sizeof line, "event split-line-%u", rndn(100000));
            write_all(wfd, line, (size_t)ll);
            /* remember the text so the second half completes the same line */
            spit("pending.txt", line, (size_t)ll);
            break;
        case 3: {
            size_t n;
            unsigned char *b = slurp("pending.txt", &n);
            write_all(wfd, " tail-part\n", 11);
            unsigned char full[128];
            memcpy(full, b, n);
            memcpy(full + n, " tail-part", 10);
            exp_crc = crc32_update(exp_crc, full, n + 10);
            exp_lines++;
            free(b);
            unlink("pending.txt");
            break;
        }
        case 5:
            check(ftruncate(wfd, 0) == 0, "truncate");
            break;
        case 7:
            write_all(wfd, "half line without newline", 25);
            break;
        default:
            break;
        }
        if (round == 7) {
            /* wake first so the follower buffers the partial line, then truncate under it */
            unsigned char w = 'W';
            write_all(wake[1], &w, 1);
            char a[64];
            read_line(ack[0], a, sizeof a);
            long nl, pl;
            int tr;
            check(sscanf(a, "%ld %ld %d", &nl, &pl, &tr) == 3, "ack parse");
            printf("round 7a: half line buffered, new=%ld partial=%ld truncated=%d\n", nl, pl, tr);
            check(ftruncate(wfd, 0) == 0, "rotate truncate");
        }
        unsigned char w = 'W';
        write_all(wake[1], &w, 1);
        char a[64];
        read_line(ack[0], a, sizeof a);
        long nl, pl;
        int tr;
        check(sscanf(a, "%ld %ld %d", &nl, &pl, &tr) == 3, "ack parse");
        check(nl == expect_new[round], "delivered lines this round");
        printf("round %d (%s): new=%ld partial=%ld truncated=%d\n", round, desc[round], nl, pl, tr);
        if (round == 5 || round == 7) {
            check(tr == 1, "truncation noticed");
        }
    }
    unsigned char q = 'Q';
    write_all(wake[1], &q, 1);
    char endl[16];
    read_line(ack[0], endl, sizeof endl);
    check(!strcmp(endl, "END"), "end marker");
    unsigned char rep[12];
    size_t got = 0;
    while (got < 12) {
        ssize_t r = read(ack[0], rep + got, 12 - got);
        check(r > 0, "report");
        got += (size_t)r;
    }
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "follower exit");
    close(wake[1]);
    close(ack[0]);
    close(wfd);
    printf("follower saw lines=%u truncations=%u crc matches writer=%s\n", get32(rep), get32(rep + 4),
           get32(rep + 8) == exp_crc ? "yes" : "no");
    check(get32(rep) == (uint32_t)exp_lines && get32(rep + 8) == exp_crc, "totals");
    unlink("app.log");
    return 0;
}
