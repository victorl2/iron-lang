/*
 * title: wc style word line and byte counter
 * topic: io_files
 * covers: streaming counters over generated text, longest line, character classes, fread block scanning, cross-check
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long lines, words, bytes, maxline, digits, upper, spaces;
} Counts;

static unsigned rs = 99991u;
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

/* counts characters one at a time */
static Counts count_chars(FILE *f) {
    Counts c = {0};
    int ch, inword = 0;
    long cur = 0;
    while ((ch = fgetc(f)) != EOF) {
        c.bytes++;
        if (ch == '\n') {
            c.lines++;
            if (cur > c.maxline)
                c.maxline = cur;
            cur = 0;
        } else {
            cur++;
        }
        if (isspace(ch)) {
            inword = 0;
            c.spaces++;
        } else if (!inword) {
            inword = 1;
            c.words++;
        }
        c.digits += isdigit(ch) != 0;
        c.upper += isupper(ch) != 0;
    }
    if (cur > c.maxline)
        c.maxline = cur;
    return c;
}

/* counts with fread blocks of an awkward size, carrying state across block boundaries */
static Counts count_blocks(FILE *f, size_t blk) {
    Counts c = {0};
    unsigned char *buf = malloc(blk);
    check(buf != NULL, "malloc");
    int inword = 0;
    long cur = 0;
    size_t n;
    while ((n = fread(buf, 1, blk, f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            int ch = buf[i];
            c.bytes++;
            if (ch == '\n') {
                c.lines++;
                if (cur > c.maxline)
                    c.maxline = cur;
                cur = 0;
            } else
                cur++;
            if (isspace(ch)) {
                inword = 0;
                c.spaces++;
            } else if (!inword) {
                inword = 1;
                c.words++;
            }
            c.digits += isdigit(ch) != 0;
            c.upper += isupper(ch) != 0;
        }
    }
    if (cur > c.maxline)
        c.maxline = cur;
    free(buf);
    return c;
}

int main(void) {
    static const char *vocab[] = {"the", "Iron", "stream", "buffer", "42", "seek", "A1", "file", "x", "record"};
    FILE *f = fopen("text.txt", "w");
    check(f != NULL, "open");
    long exp_words = 0, exp_lines = 0;
    for (int line = 0; line < 200; line++) {
        int nw = (int)(rnd() % 12);
        for (int i = 0; i < nw; i++) {
            unsigned r = rnd();
            unsigned gap = rnd();
            fputs(vocab[r % 10], f);
            exp_words++;
            /* one to three separators, occasionally a tab */
            int seps = 1 + (int)(gap % 3);
            for (int s = 0; s < seps; s++)
                fputc((gap >> 4) % 7 == 0 ? '\t' : ' ', f);
        }
        fputc('\n', f);
        exp_lines++;
    }
    fputs("last line without newline", f);
    exp_words += 4;
    fclose(f);

    f = fopen("text.txt", "r");
    Counts a = count_chars(f);
    fclose(f);
    printf("lines %ld words %ld bytes %ld\n", a.lines, a.words, a.bytes);
    printf("longest line %ld, digits %ld, uppercase %ld, whitespace %ld\n", a.maxline, a.digits,
           a.upper, a.spaces);
    check(a.lines == exp_lines, "lines vs generator");
    check(a.words == exp_words, "words vs generator");

    size_t blocks[] = {1, 7, 100, 4096};
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        f = fopen("text.txt", "rb");
        Counts b = count_blocks(f, blocks[i]);
        fclose(f);
        int same = b.lines == a.lines && b.words == a.words && b.bytes == a.bytes &&
                   b.maxline == a.maxline && b.digits == a.digits && b.upper == a.upper &&
                   b.spaces == a.spaces;
        printf("block size %4zu -> identical counts: %s\n", blocks[i], same ? "yes" : "NO");
        check(same, "block counts");
    }

    /* file size via seek agrees with the byte count */
    f = fopen("text.txt", "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    check(size == a.bytes, "size");
    remove("text.txt");
    return 0;
}
