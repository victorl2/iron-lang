/*
 * title: tail -n by seeking backwards
 * topic: io_files
 * covers: backward scanning in blocks, newline counting, tail of short files, N larger than file, no trailing newline
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Find the offset where the last n lines of f begin, scanning backwards in blocks. */
static long tail_offset(FILE *f, int n, size_t blk) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size == 0 || n <= 0)
        return size;
    unsigned char *buf = malloc(blk);
    check(buf != NULL, "alloc");
    /* a trailing newline ends the last line rather than starting an empty one */
    long limit = size;
    unsigned char last;
    fseek(f, size - 1, SEEK_SET);
    check(fread(&last, 1, 1, f) == 1, "last byte");
    if (last == '\n')
        limit--;
    int seen = 0;
    long pos = limit;
    long result = 0;
    int found = 0;
    while (pos > 0 && !found) {
        size_t chunk = (size_t)(pos > (long)blk ? (long)blk : pos);
        pos -= (long)chunk;
        fseek(f, pos, SEEK_SET);
        check(fread(buf, 1, chunk, f) == chunk, "block");
        for (size_t i = chunk; i > 0; i--) {
            if (buf[i - 1] == '\n') {
                seen++;
                if (seen == n) {
                    result = pos + (long)i; /* first byte after this newline */
                    found = 1;
                    break;
                }
            }
        }
    }
    free(buf);
    return found ? result : 0;
}

static void write_lines(const char *name, int count, int trailing) {
    FILE *f = fopen(name, "w");
    check(f != NULL, "open");
    for (int i = 1; i <= count; i++) {
        fprintf(f, "line %d %.*s", i, (i * 7) % 23, "abcdefghijklmnopqrstuvwxyz");
        if (i < count || trailing)
            fputc('\n', f);
    }
    fclose(f);
}

int main(void) {
    int counts[] = {0, 1, 5, 50, 500};
    int wants[] = {1, 3, 10, 600};
    size_t blocks[] = {1, 16, 4096};
    for (size_t ci = 0; ci < sizeof counts / sizeof counts[0]; ci++) {
        for (int trailing = 0; trailing <= 1; trailing++) {
            if (counts[ci] == 0 && trailing == 0)
                continue;
            write_lines("t.txt", counts[ci], trailing);
            for (size_t wi = 0; wi < sizeof wants / sizeof wants[0]; wi++) {
                int want = wants[wi];
                long first_off = -1;
                for (size_t bi = 0; bi < sizeof blocks / sizeof blocks[0]; bi++) {
                    FILE *f = fopen("t.txt", "rb");
                    long off = tail_offset(f, want, blocks[bi]);
                    if (first_off < 0)
                        first_off = off;
                    check(off == first_off, "block size independent");

                    /* verify by counting lines from the offset and via forward scan */
                    fseek(f, off, SEEK_SET);
                    char line[128];
                    int got = 0, firstno = -1, lastno = -1;
                    while (fgets(line, sizeof line, f)) {
                        int no = 0;
                        check(sscanf(line, "line %d", &no) == 1, "parse");
                        if (firstno < 0)
                            firstno = no;
                        lastno = no;
                        got++;
                    }
                    fclose(f);
                    int expect = want < counts[ci] ? want : counts[ci];
                    check(got == expect, "tail line count");
                    if (bi == 0) {
                        printf("lines=%3d trailing=%d tail -n %3d: offset %5ld, %3d lines", counts[ci],
                               trailing, want, off, got);
                        if (got > 0)
                            printf(" (%d..%d)", firstno, lastno);
                        printf("\n");
                    }
                }
            }
        }
    }
    remove("t.txt");
    return 0;
}
