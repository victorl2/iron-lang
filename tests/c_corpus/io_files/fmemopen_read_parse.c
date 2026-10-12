/*
 * title: fmemopen memory streams for parsing
 * topic: io_files
 * covers: fmemopen read mode, write mode NUL termination, fgets/fscanf on buffers, seek in memory
 * deps: posix
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

int main(void) {
    /* read stream over a string literal copy */
    char text[] = "10 20 30\n40 50\nname=zed\n";
    FILE *m = fmemopen(text, strlen(text), "r");
    check(m != NULL, "fmemopen r");
    int a, b, c;
    check(fscanf(m, "%d %d %d", &a, &b, &c) == 3, "row1");
    printf("row 1 sum %d\n", a + b + c);
    check(fgetc(m) == '\n', "newline");
    check(fscanf(m, "%d %d", &a, &b) == 2, "row2");
    printf("row 2 product %d\n", a * b);
    long pos = ftell(m);
    printf("position after two rows %ld\n", pos);
    check(pos == 14, "pos");
    fseek(m, 0, SEEK_END);
    printf("stream length %ld\n", ftell(m));
    check(fgetc(m) == EOF && feof(m), "eof at end of buffer");
    fseek(m, -9, SEEK_END);
    char line[32];
    check(fgets(line, sizeof line, m) != NULL, "tail line");
    line[strcspn(line, "\n")] = 0;
    printf("tail line \"%s\"\n", line);
    fclose(m);

    /* write stream: output is NUL terminated when flushed or closed */
    char buf[32];
    memset(buf, 'x', sizeof buf);
    m = fmemopen(buf, sizeof buf, "w");
    check(m != NULL, "fmemopen w");
    fprintf(m, "%d+%d=%d", 2, 3, 5);
    fflush(m);
    printf("after flush buffer holds \"%s\"\n", buf);
    check(strcmp(buf, "2+3=5") == 0, "flushed content");
    fclose(m);

    /* line splitter over a memory stream, counting words */
    char doc[] = "the quick brown fox\njumps over\n\nthe lazy dog\n";
    m = fmemopen(doc, strlen(doc), "r");
    check(m != NULL, "doc");
    int lines = 0, words = 0, longest = 0;
    while (fgets(line, sizeof line, m)) {
        int inword = 0, len = (int)strcspn(line, "\n");
        for (int i = 0; i < len; i++) {
            if (line[i] != ' ' && !inword)
                words++;
            inword = line[i] != ' ';
        }
        if (len > longest)
            longest = len;
        lines++;
    }
    fclose(m);
    printf("lines %d words %d longest %d\n", lines, words, longest);
    check(lines == 4 && words == 9 && longest == 19, "doc stats");

    /* read-write update in place (only the patched prefix is checked: the tail after
     * the write position is implementation-defined when the stream is closed) */
    char rec[] = "AAAA-BBBB-CCCC";
    m = fmemopen(rec, sizeof rec, "r+");
    check(m != NULL, "r+");
    fseek(m, 5, SEEK_SET);
    fputs("zzzz", m);
    fclose(m);
    printf("patched record %.9s\n", rec);
    check(strncmp(rec, "AAAA-zzzz", 9) == 0, "patched");
    return 0;
}
