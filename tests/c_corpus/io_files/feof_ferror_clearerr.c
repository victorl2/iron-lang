/*
 * title: feof ferror and clearerr handling
 * topic: io_files
 * covers: EOF flag stickiness, ferror on wrong-direction I/O, clearerr, reading a growing file
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

static void flags(const char *tag, FILE *f) {
    printf("%-22s eof=%d error=%d\n", tag, feof(f) != 0, ferror(f) != 0);
}

int main(void) {
    FILE *w = fopen("grow.txt", "w");
    check(w != NULL, "open w");
    setvbuf(w, NULL, _IONBF, 0);
    fputs("ab", w);

    FILE *r = fopen("grow.txt", "r");
    check(r != NULL, "open r");
    flags("fresh reader", r);
    check(fgetc(r) == 'a' && fgetc(r) == 'b', "two chars");
    flags("after two chars", r);
    int c = fgetc(r);
    check(c == EOF, "eof");
    flags("after reading past end", r);

    /* the EOF flag is sticky: new data appended later is not seen until cleared */
    fputs("cd", w);
    c = fgetc(r);
    printf("sticky eof read returns %s\n", c == EOF ? "EOF" : "data");
    clearerr(r);
    flags("after clearerr", r);
    c = fgetc(r);
    printf("after clearerr read '%c'\n", c);
    check(c == 'c', "sees appended data");
    check(fgetc(r) == 'd', "d");
    check(fgetc(r) == EOF && feof(r), "end again");
    fclose(w);
    clearerr(r);

    /* tail-follow loop: poll for new lines with clearerr between rounds */
    w = fopen("grow.txt", "a");
    setvbuf(w, NULL, _IONBF, 0);
    int rounds = 0, got = 0;
    char line[32];
    for (int i = 0; i < 4; i++) {
        fprintf(w, "\nrecord %d\n", i);
        rounds++;
        while (fgets(line, sizeof line, r)) {
            if (line[0] == 'r')
                got++;
        }
        clearerr(r);
    }
    printf("follow: %d rounds, %d records seen\n", rounds, got);
    check(got == 4, "follow");
    fclose(w);
    fclose(r);

    /* wrong-direction I/O sets the error flag, not EOF */
    w = fopen("grow.txt", "w");
    c = fgetc(w);
    flags("read from w stream", w);
    check(c == EOF && ferror(w) && !feof(w), "ferror on read of w");
    clearerr(w);
    flags("after clearerr", w);
    fclose(w);

    r = fopen("grow.txt", "r");
    c = fputc('x', r);
    flags("write to r stream", r);
    check(c == EOF && ferror(r), "ferror on write to r");
    fclose(r);

    /* fread reporting: short count plus feof, versus an error */
    w = fopen("five.bin", "wb");
    check(fwrite("12345", 1, 5, w) == 5, "five");
    fclose(w);
    r = fopen("five.bin", "rb");
    char buf[16];
    size_t n = fread(buf, 1, sizeof buf, r);
    printf("fread asked 16 got %zu, eof=%d error=%d\n", n, feof(r) != 0, ferror(r) != 0);
    check(n == 5 && feof(r) && !ferror(r), "short fread");
    n = fread(buf, 1, sizeof buf, r);
    printf("second fread got %zu\n", n);
    check(n == 0, "zero");

    /* fseek clears the EOF flag */
    check(fseek(r, 1, SEEK_SET) == 0, "seek");
    flags("after fseek", r);
    check(!feof(r), "fseek clears eof");
    fclose(r);

    /* an empty file reports EOF on the first read */
    w = fopen("empty.txt", "w");
    fclose(w);
    r = fopen("empty.txt", "r");
    check(fgets(buf, sizeof buf, r) == NULL, "empty fgets");
    flags("empty file", r);
    fclose(r);

    remove("grow.txt");
    remove("five.bin");
    remove("empty.txt");
    return 0;
}
