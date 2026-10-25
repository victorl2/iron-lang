/*
 * title: Line reassembly across arbitrary read chunk boundaries
 * topic: io_files
 * covers: read into small buffer, carry-over buffer, lines split across reads, overlong lines, missing final newline, pipe writer with irregular chunks
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


static unsigned long long xs_state = 88172645463325252ULL;
static inline unsigned long long xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}
static inline unsigned rnd_below(unsigned n) {
    unsigned v = (unsigned)(xs() >> 33);
    return v % n;
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

/* write everything, retrying short writes; 0 on success, -1 on error */
static inline int wr_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
static inline int wait_exit(pid_t p) {
    int st = 0;
    while (waitpid(p, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? 1 : 0);
}
#define MAXLINE 40

typedef struct {
    int fd;
    unsigned char buf[16];
    size_t pos, len;
    int eof;
} Reader;

static int next_byte(Reader *r) {
    if (r->pos == r->len) {
        if (r->eof) return -1;
        ssize_t n;
        do n = read(r->fd, r->buf, sizeof r->buf); while (n < 0 && errno == EINTR);
        CHECK(n >= 0);
        if (n == 0) { r->eof = 1; return -1; }
        r->len = (size_t)n;
        r->pos = 0;
    }
    return r->buf[r->pos++];
}

/*
 * Read one line of at most MAXLINE bytes into out (no newline stored).
 * Returns 1 for a complete line, 2 when the line was cut at MAXLINE (rest continues in the next call),
 * 3 for a final unterminated line, 0 at end of input.
 */
static int read_line(Reader *r, char *out, size_t *len) {
    size_t n = 0;
    for (;;) {
        int c = next_byte(r);
        if (c < 0) {
            *len = n;
            return n ? 3 : 0;
        }
        if (c == '\n') { *len = n; return 1; }
        out[n++] = (char)c;
        if (n == MAXLINE) { *len = n; return 2; }
    }
}

static void feeder(int wfd, const char *text, size_t total, unsigned seed) {
    xs_state = seed;
    size_t off = 0;
    while (off < total) {
        size_t n = 1 + rnd_below(23);
        if (n > total - off) n = total - off;
        if (wr_all(wfd, text + off, n) != 0) _exit(2);
        off += n;
        if (rnd_below(4) == 0) usleep(50);
    }
    close(wfd);
    _exit(0);
}

int main(void) {
    /* build a text with short lines, empty lines, one overlong line, and no final newline */
    static char text[4000];
    size_t tl = 0;
    for (int i = 0; i < 40; i++) {
        unsigned len = i == 17 ? 95 : rnd_below(30);
        for (unsigned k = 0; k < len; k++) text[tl++] = (char)('a' + (i + (int)k) % 26);
        if (i < 39) text[tl++] = '\n';
    }
    text[tl] = 0;

    static const unsigned seeds[3] = {1, 77, 12345};
    unsigned long long first_digest = 0;
    for (int run = 0; run < 3; run++) {
        int p[2];
        CHECK(pipe(p) == 0);
        pid_t c = fork();
        CHECK(c >= 0);
        if (c == 0) { close(p[0]); feeder(p[1], text, tl, seeds[run]); }
        close(p[1]);
        Reader r = {p[0], {0}, 0, 0, 0};
        char line[MAXLINE + 1];
        size_t len;
        int kind;
        int complete = 0, cut = 0, unterminated = 0, empty = 0;
        size_t bytes = 0;
        unsigned long long digest = FNV0;
        while ((kind = read_line(&r, line, &len)) != 0) {
            bytes += len;
            if (kind == 1) { complete++; if (len == 0) empty++; bytes++; }
            else if (kind == 2) cut++;
            else unterminated++;
            digest = fnv(digest, line, len);
            digest = fnv(digest, &kind, 1);
        }
        close(p[0]);
        CHECK(wait_exit(c) == 0);
        printf("run %d: complete=%d empty=%d cut=%d unterminated=%d bytes=%zu digest=%016llx\n", run, complete, empty, cut, unterminated, bytes, digest);
        CHECK(bytes == tl);
        if (run == 0) first_digest = digest;
        else CHECK(digest == first_digest); /* chunking must not change the result */
    }
    puts("chunk boundaries never change the parsed lines");

    /* a few hand-made edge cases fed through a pipe in one piece */
    static const struct { const char *in; const char *want; } cases[] = {
        {"", ""},
        {"\n", "1:0|"},
        {"abc", "3:3|"},
        {"abc\n", "1:3|"},
        {"a\n\nb\n", "1:1|1:0|1:1|"},
        {"0123456789012345678901234567890123456789\n", "2:40|1:0|"},
        {"01234567890123456789012345678901234567890", "2:40|3:1|"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int p[2];
        CHECK(pipe(p) == 0);
        CHECK(wr_all(p[1], cases[i].in, strlen(cases[i].in)) == 0);
        close(p[1]);
        Reader r = {p[0], {0}, 0, 0, 0};
        char line[MAXLINE + 1], desc[128] = "";
        size_t len;
        int kind;
        while ((kind = read_line(&r, line, &len)) != 0) {
            char part[24];
            snprintf(part, sizeof part, "%d:%zu|", kind, len);
            strncat(desc, part, sizeof desc - strlen(desc) - 1);
        }
        close(p[0]);
        printf("case %u -> '%s'\n", i, desc);
        CHECK(strcmp(desc, cases[i].want) == 0);
    }
    return 0;
}
