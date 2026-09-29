/*
 * title: setvbuf full line and unbuffered modes
 * topic: io_files
 * covers: setvbuf _IOFBF/_IOLBF/_IONBF, visibility through a second reader, fflush
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

/* how many bytes can an independent reader see right now */
static long visible(const char *name) {
    FILE *r = fopen(name, "rb");
    check(r != NULL, "reader");
    fseek(r, 0, SEEK_END);
    long n = ftell(r);
    fclose(r);
    return n;
}

static char bufs[3][8192];

int main(void) {
    /* fully buffered: nothing visible until flush (buffer is 8192 bytes) */
    FILE *f = fopen("vb_full.txt", "w");
    check(f != NULL, "open full");
    check(setvbuf(f, bufs[0], _IOFBF, sizeof bufs[0]) == 0, "setvbuf full");
    fputs("hello\nworld\n", f);
    long v1 = visible("vb_full.txt");
    fflush(f);
    long v2 = visible("vb_full.txt");
    fputs("more", f);
    long v3 = visible("vb_full.txt");
    fclose(f);
    long v4 = visible("vb_full.txt");
    printf("full: %ld %ld %ld %ld\n", v1, v2, v3, v4);
    check(v1 == 0 && v2 == 12 && v3 == 12 && v4 == 16, "full");

    /* line buffered: complete lines become visible, the partial tail does not */
    f = fopen("vb_line.txt", "w");
    check(setvbuf(f, bufs[1], _IOLBF, sizeof bufs[1]) == 0, "setvbuf line");
    fputs("abc", f);
    long l1 = visible("vb_line.txt");
    fputs("def\n", f);
    long l2 = visible("vb_line.txt");
    fputs("gh", f);
    long l3 = visible("vb_line.txt");
    fputs("i\nj", f);
    long l4 = visible("vb_line.txt");
    fclose(f);
    long l5 = visible("vb_line.txt");
    printf("line: %ld %ld %ld %ld %ld\n", l1, l2, l3, l4, l5);
    check(l1 == 0 && l2 == 7 && l3 == 7 && l4 == 11 && l5 == 12, "line");

    /* unbuffered: every byte visible immediately */
    f = fopen("vb_none.txt", "w");
    check(setvbuf(f, NULL, _IONBF, 0) == 0, "setvbuf none");
    long n1 = visible("vb_none.txt");
    fputc('x', f);
    long n2 = visible("vb_none.txt");
    fputs("yz", f);
    long n3 = visible("vb_none.txt");
    fprintf(f, "%d", 12345);
    long n4 = visible("vb_none.txt");
    fclose(f);
    printf("none: %ld %ld %ld %ld\n", n1, n2, n3, n4);
    check(n1 == 0 && n2 == 1 && n3 == 3 && n4 == 8, "none");

    /* content is identical regardless of buffering mode */
    const char *files[] = {"vb_full.txt", "vb_line.txt", "vb_none.txt"};
    for (int i = 0; i < 3; i++) {
        f = fopen(files[i], "r");
        char b[32];
        size_t n = fread(b, 1, sizeof b - 1, f);
        b[n] = 0;
        for (size_t k = 0; k < n; k++)
            if (b[k] == '\n')
                b[k] = '|';
        printf("%s: [%s]\n", files[i], b);
        fclose(f);
    }

    /* large write in full mode: data beyond the buffer size reaches the file before flush */
    f = fopen("vb_big.txt", "w");
    check(setvbuf(f, bufs[2], _IOFBF, 1024) == 0, "setvbuf big");
    for (int i = 0; i < 300; i++)
        fputs("0123456789", f);
    long b1 = visible("vb_big.txt");
    fflush(f);
    long b2 = visible("vb_big.txt");
    fclose(f);
    printf("big: flushed-early=%s final=%ld\n", (b1 > 0 && b1 < 3000) ? "partial" : "other", b2);
    check(b1 > 0 && b1 < 3000 && b2 == 3000, "big");

    remove("vb_full.txt");
    remove("vb_line.txt");
    remove("vb_none.txt");
    remove("vb_big.txt");
    return 0;
}
