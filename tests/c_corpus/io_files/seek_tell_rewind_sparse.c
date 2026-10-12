/*
 * title: fseek ftell rewind and sparse writes
 * topic: io_files
 * covers: SEEK_SET/CUR/END, ftell, rewind, seeking past EOF, zero fill, error return
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

int main(void) {
    FILE *f = fopen("seek.dat", "w+b");
    check(f != NULL, "open");

    printf("initial tell %ld\n", ftell(f));
    const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    check(fwrite(alpha, 1, 26, f) != 0, "fwrite");
    printf("after write tell %ld\n", ftell(f));

    check(fseek(f, 0, SEEK_SET) == 0, "seek set");
    int c = fgetc(f);
    printf("first %c tell %ld\n", c, ftell(f));

    check(fseek(f, 9, SEEK_CUR) == 0, "seek cur");
    c = fgetc(f);
    printf("cur+9 %c tell %ld\n", c, ftell(f));

    check(fseek(f, -3, SEEK_END) == 0, "seek end");
    c = fgetc(f);
    printf("end-3 %c tell %ld\n", c, ftell(f));

    check(fseek(f, -1, SEEK_CUR) == 0, "back one");
    c = fgetc(f);
    printf("cur-1 %c tell %ld\n", c, ftell(f));

    /* seek before start fails and leaves position alone */
    long before = ftell(f);
    int rc = fseek(f, -100, SEEK_SET);
    printf("seek to -100 %s, tell unchanged %s\n", rc != 0 ? "failed" : "worked",
           ftell(f) == before ? "yes" : "no");
    check(rc != 0 && ftell(f) == before, "negative seek");

    /* reading at EOF then rewind clears EOF */
    check(fseek(f, 0, SEEK_END) == 0, "to end");
    c = fgetc(f);
    printf("read at end -> %s, feof=%d\n", c == EOF ? "EOF" : "data", feof(f) != 0);
    check(c == EOF && feof(f), "eof");
    rewind(f);
    check(!feof(f), "rewind clears eof");
    c = fgetc(f);
    printf("after rewind %c tell %ld\n", c, ftell(f));

    /* write far past the end: the gap reads as zero bytes */
    check(fseek(f, 100, SEEK_SET) == 0, "seek past end");
    fputs("<END>", f);
    fflush(f);
    check(fseek(f, 0, SEEK_END) == 0, "size");
    long size = ftell(f);
    printf("size after sparse write %ld\n", size);
    check(size == 105, "sparse size");

    rewind(f);
    unsigned char buf[105];
    check(fread(buf, 1, sizeof buf, f) == sizeof buf, "read all");
    int zeros = 0, letters = 0;
    for (int i = 0; i < 100; i++) {
        if (buf[i] == 0)
            zeros++;
        else
            letters++;
    }
    printf("first 100 bytes: %d letters, %d zeros\n", letters, zeros);
    check(letters == 26 && zeros == 74, "gap content");
    printf("tail \"%.5s\"\n", (const char *)(buf + 100));

    /* overwrite in the middle and read around it */
    fseek(f, 10, SEEK_SET);
    fputs("--", f);
    fseek(f, 8, SEEK_SET);
    char win[9] = {0};
    check(fread(win, 1, 8, f) != 0, "fread");
    printf("window \"%s\"\n", win);
    check(memcmp(win, "IJ--MNOP", 8) == 0, "window bytes");

    /* fgetpos-free position arithmetic: walk backwards by 5 */
    long pos = 25;
    int count = 0;
    while (pos >= 0) {
        fseek(f, pos, SEEK_SET);
        int ch = fgetc(f);
        if (count < 6)
            printf("%c", ch);
        pos -= 5;
        count++;
    }
    printf("\nstepped %d times\n", count);

    fclose(f);
    check(remove("seek.dat") == 0, "remove");
    return 0;
}
