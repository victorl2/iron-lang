/*
 * title: File-backed stack driving an RPN evaluator
 * topic: io_files
 * covers: stack on a file, push via pwrite at end, pop via pread plus ftruncate, underflow detection, model comparison, expression evaluation
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

static inline long fsize(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (long)st.st_size;
}
#define FRAME 8

typedef struct { int fd; long depth; long max_depth; } FStack;

static void put64(unsigned char *p, long long v) {
    unsigned long long u = (unsigned long long)v;
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(u >> (8 * i));
}
static long long get64(const unsigned char *p) {
    unsigned long long u = 0;
    for (int i = 0; i < 8; i++) u |= (unsigned long long)p[i] << (8 * i);
    return (long long)u;
}

static void push(FStack *s, long long v) {
    unsigned char b[FRAME];
    put64(b, v);
    CHECK(pwrite(s->fd, b, FRAME, (off_t)s->depth * FRAME) == FRAME);
    s->depth++;
    if (s->depth > s->max_depth) s->max_depth = s->depth;
}

static int pop(FStack *s, long long *v) {
    if (s->depth == 0) return -1;
    unsigned char b[FRAME];
    s->depth--;
    CHECK(pread(s->fd, b, FRAME, (off_t)s->depth * FRAME) == FRAME);
    CHECK(ftruncate(s->fd, (off_t)s->depth * FRAME) == 0);
    *v = get64(b);
    return 0;
}

static int peek(FStack *s, long long *v) {
    if (s->depth == 0) return -1;
    unsigned char b[FRAME];
    CHECK(pread(s->fd, b, FRAME, (off_t)(s->depth - 1) * FRAME) == FRAME);
    *v = get64(b);
    return 0;
}

/* evaluate a space-separated RPN expression; returns 0 and the value, or an error code */
static int eval_rpn(FStack *s, const char *expr, long long *out) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", expr);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        if ((tok[0] >= '0' && tok[0] <= '9') || (tok[0] == '-' && tok[1])) {
            push(s, atoll(tok));
            continue;
        }
        long long a, b;
        if (pop(s, &b) != 0 || pop(s, &a) != 0) return 1; /* underflow */
        long long r;
        switch (tok[0]) {
        case '+': r = a + b; break;
        case '-': r = a - b; break;
        case '*': r = a * b; break;
        case '/':
            if (b == 0) return 2;
            r = a / b;
            break;
        case '%':
            if (b == 0) return 2;
            r = a % b;
            break;
        default: return 3;
        }
        push(s, r);
    }
    long long top;
    if (pop(s, &top) != 0) return 1;
    if (s->depth != 0) return 4; /* leftover operands */
    *out = top;
    return 0;
}

static const char *errname(int e) {
    switch (e) {
    case 1: return "stack underflow";
    case 2: return "division by zero";
    case 3: return "unknown operator";
    case 4: return "leftover operands";
    default: return "ok";
    }
}

int main(void) {
    FStack s;
    s.fd = open("stack.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(s.fd >= 0);
    s.depth = 0; s.max_depth = 0;

    static const struct { const char *expr; long long want; int err; } cases[] = {
        {"3 4 +", 7, 0},
        {"5 1 2 + 4 * + 3 -", 14, 0},
        {"2 3 4 * +", 14, 0},
        {"100 7 / 3 %", 2, 0},
        {"-5 -6 *", 30, 0},
        {"1 2 3 4 5 + + + +", 15, 0},
        {"9 0 /", 0, 2},
        {"1 +", 0, 1},
        {"1 2 ?", 0, 3},
        {"1 2 3 +", 0, 4},
        {"", 0, 1},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long long v = 0;
        int e = eval_rpn(&s, cases[i].expr, &v);
        if (e == 0) printf("'%s' = %lld\n", cases[i].expr, v);
        else printf("'%s' -> %s\n", cases[i].expr, errname(e));
        CHECK(e == cases[i].err && (e != 0 || v == cases[i].want));
        /* reset for the next expression (a failed evaluation may leave frames) */
        CHECK(ftruncate(s.fd, 0) == 0);
        s.depth = 0;
    }
    printf("deepest stack seen: %ld frames\n", s.max_depth);
    CHECK(s.max_depth == 5);

    /* random push/pop/peek against an in-memory model */
    static long long model[600];
    long mtop = 0;
    long pushes = 0, pops = 0, underflows = 0;
    for (int i = 0; i < 1500; i++) {
        unsigned op = rnd_below(10);
        long long v = (long long)(xs() >> 20) - (1LL << 43);
        if (op < 5) {
            if (mtop < 600) { push(&s, v); model[mtop++] = v; pushes++; }
        } else if (op < 9) {
            long long got;
            int rc = pop(&s, &got);
            if (mtop == 0) { CHECK(rc != 0); underflows++; }
            else { CHECK(rc == 0 && got == model[--mtop]); pops++; }
        } else {
            long long got;
            int rc = peek(&s, &got);
            CHECK((rc == 0) == (mtop > 0));
            if (rc == 0) CHECK(got == model[mtop - 1]);
        }
        CHECK(s.depth == mtop && fsize(s.fd) == mtop * FRAME);
    }
    printf("random ops: pushes=%ld pops=%ld underflows=%ld final depth=%ld\n", pushes, pops, underflows, s.depth);

    /* drain and verify LIFO order end to end */
    long long expect_sum = 0, got_sum = 0;
    for (long i = 0; i < mtop; i++) expect_sum += model[i] % 1000;
    long long v;
    while (pop(&s, &v) == 0) got_sum += v % 1000;
    CHECK(got_sum == expect_sum && fsize(s.fd) == 0);
    printf("drained checksum matches: %d, file size now %ld\n", got_sum == expect_sum, fsize(s.fd));
    close(s.fd);
    unlink("stack.bin");
    return 0;
}
