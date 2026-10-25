/*
 * title: Reverse a file line by line from the end
 * topic: io_files
 * covers: backward block reads with fseek, line boundary detection across blocks, missing final newline, offset index alternative
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rs = 5150u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Write the lines of `in` in reverse order to `out`, reading blocks backwards. */
static long reverse_lines(FILE *in, FILE *out, size_t blk) {
    fseek(in, 0, SEEK_END);
    long end = ftell(in);
    if (end == 0)
        return 0;
    unsigned char *buf = malloc(blk);
    check(buf != NULL, "alloc");
    long lines = 0;
    long line_end = end; /* exclusive end of the line being assembled */
    long pos = end;
    while (pos > 0) {
        size_t n = (size_t)(pos > (long)blk ? (long)blk : pos);
        pos -= (long)n;
        check(fseek(in, pos, SEEK_SET) == 0, "seek");
        check(fread(buf, 1, n, in) == n, "read block");
        for (size_t i = n; i > 0; i--) {
            long here = pos + (long)i - 1; /* offset of buf[i-1] */
            if (buf[i - 1] == '\n' && here != end - 1) {
                /* line starts at here+1 */
                long start = here + 1;
                size_t len = (size_t)(line_end - start);
                unsigned char *tmp = malloc(len);
                check(tmp != NULL, "tmp");
                fseek(in, start, SEEK_SET);
                check(fread(tmp, 1, len, in) == len, "line read");
                fwrite(tmp, 1, len, out);
                if (tmp[len - 1] != '\n')
                    fputc('\n', out);
                free(tmp);
                lines++;
                line_end = start;
            }
        }
    }
    /* first line of the file */
    size_t len = (size_t)line_end;
    unsigned char *tmp = malloc(len);
    check(tmp != NULL, "tmp");
    fseek(in, 0, SEEK_SET);
    check(fread(tmp, 1, len, in) == len, "first read");
    fwrite(tmp, 1, len, out);
    if (tmp[len - 1] != '\n')
        fputc('\n', out);
    free(tmp);
    lines++;
    free(buf);
    return lines;
}

int main(void) {
    enum { NL = 120 };
    char *ref[NL];
    for (int trailing = 1; trailing >= 0; trailing--) {
        FILE *f = fopen("fwd.txt", "w");
        check(f != NULL, "open");
        for (int i = 0; i < NL; i++) {
            int len = (int)(rnd() % 40);
            char *l = malloc((size_t)len + 32);
            int o = snprintf(l, 16, "%03d:", i);
            for (int k = 0; k < len; k++)
                l[o++] = (char)('a' + (int)(rnd() % 26));
            l[o] = 0;
            ref[i] = l;
            fputs(l, f);
            if (i != NL - 1 || trailing)
                fputc('\n', f);
        }
        fclose(f);

        size_t blocks[] = {1, 7, 64, 4096};
        for (size_t b = 0; b < 4; b++) {
            FILE *in = fopen("fwd.txt", "rb");
            FILE *out = fopen("rev.txt", "wb");
            long n = reverse_lines(in, out, blocks[b]);
            fclose(in);
            fclose(out);
            check(n == NL, "line count");

            /* verify: k-th line of the output is ref[NL-1-k] */
            out = fopen("rev.txt", "r");
            char line[128];
            int k = 0, bad = 0;
            while (fgets(line, sizeof line, out)) {
                line[strcspn(line, "\n")] = 0;
                if (k >= NL || strcmp(line, ref[NL - 1 - k]) != 0)
                    bad++;
                k++;
            }
            fclose(out);
            printf("trailing newline %d, block %4zu: %ld lines, mismatches %d\n", trailing,
                   blocks[b], n, bad);
            check(k == NL && bad == 0, "reversed");
        }
        if (trailing) {
            FILE *out = fopen("rev.txt", "r");
            char line[128];
            check(fgets(line, sizeof line, out) != NULL, "first");
            printf("first output line: %s", line);
            fclose(out);
        }
        for (int i = 0; i < NL; i++)
            free(ref[i]);
    }

    /* an empty file reverses to an empty file; a one-line file to itself */
    FILE *f = fopen("fwd.txt", "w");
    fclose(f);
    FILE *in = fopen("fwd.txt", "rb"), *out = fopen("rev.txt", "wb");
    long n = reverse_lines(in, out, 16);
    fclose(in);
    fclose(out);
    printf("empty file: %ld lines\n", n);
    check(n == 0, "empty");

    remove("fwd.txt");
    remove("rev.txt");
    return 0;
}
