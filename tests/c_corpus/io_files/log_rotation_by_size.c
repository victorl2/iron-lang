/*
 * title: Size-based log rotation with rename
 * topic: io_files
 * covers: append logging, ftell size check, rename chain app.log.N, remove oldest, reading rotated files in order
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXSIZE = 200, KEEP = 3 };

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int exists(const char *n) {
    FILE *f = fopen(n, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static long fsize(const char *n) {
    FILE *f = fopen(n, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    return s;
}

static void name_of(char *out, size_t cap, int gen) {
    if (gen == 0)
        snprintf(out, cap, "app.log");
    else
        snprintf(out, cap, "app.log.%d", gen);
}

static int rotations = 0;

static void rotate(void) {
    char a[32], b[32];
    name_of(a, sizeof a, KEEP);
    remove(a); /* drop the oldest */
    for (int g = KEEP - 1; g >= 0; g--) {
        name_of(a, sizeof a, g);
        name_of(b, sizeof b, g + 1);
        if (exists(a))
            check(rename(a, b) == 0, "rename");
    }
    rotations++;
}

static void log_line(int seq, const char *msg) {
    char buf[96];
    int n = snprintf(buf, sizeof buf, "%04d %s\n", seq, msg);
    long sz = fsize("app.log");
    if (sz > 0 && sz + n > MAXSIZE)
        rotate();
    FILE *f = fopen("app.log", "a");
    check(f != NULL, "open log");
    check(fwrite(buf, 1, (size_t)n, f) == (size_t)n, "write log");
    check(fclose(f) == 0, "close log");
}

int main(void) {
    const char *msgs[] = {"started", "connection accepted", "request GET /index", "cache miss",
                          "response 200", "idle timeout", "shutdown requested", "ok"};
    for (int i = 0; i < 60; i++)
        log_line(i, msgs[(i * 5 + 1) % 8]);

    printf("rotations performed: %d\n", rotations);
    long total_lines = 0;
    int first_seq = -1, last_seq = -1;
    for (int g = 0; g <= KEEP + 1; g++) {
        char n[32];
        name_of(n, sizeof n, g);
        long sz = fsize(n);
        if (sz < 0) {
            printf("%-10s missing\n", n);
            continue;
        }
        FILE *f = fopen(n, "r");
        char line[96];
        int lines = 0, lo = 100000, hi = -1;
        while (fgets(line, sizeof line, f)) {
            int s = atoi(line);
            if (s < lo)
                lo = s;
            if (s > hi)
                hi = s;
            lines++;
        }
        fclose(f);
        printf("%-10s %3ld bytes, %2d lines, seq %d..%d\n", n, sz, lines, lo, hi);
        check(sz <= MAXSIZE, "size bound");
        total_lines += lines;
        if (g == KEEP)
            first_seq = lo;
        if (g == 0)
            last_seq = hi;
    }
    check(!exists("app.log.4"), "nothing beyond KEEP");
    check(last_seq == 59, "newest entry in the live file");

    /* read all generations oldest to newest: sequence numbers must be contiguous */
    int expect = first_seq, contiguous = 1, count = 0;
    for (int g = KEEP; g >= 0; g--) {
        char n[32], line[96];
        name_of(n, sizeof n, g);
        FILE *f = fopen(n, "r");
        check(f != NULL, "gen open");
        while (fgets(line, sizeof line, f)) {
            if (atoi(line) != expect)
                contiguous = 0;
            expect++;
            count++;
        }
        fclose(f);
    }
    printf("retained %d lines, sequence %d..%d contiguous: %s\n", count, first_seq, expect - 1,
           contiguous ? "yes" : "NO");
    check(contiguous && count == total_lines, "contiguous");
    printf("dropped %d oldest lines\n", 60 - count);

    for (int g = 0; g <= KEEP; g++) {
        char n[32];
        name_of(n, sizeof n, g);
        remove(n);
    }
    return 0;
}
