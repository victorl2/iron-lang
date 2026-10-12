/*
 * title: fflush on all streams and read write switching
 * topic: io_files
 * covers: fflush(NULL), fflush before switching direction, r+ alternation, reader visibility
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

static long visible(const char *name) {
    FILE *r = fopen(name, "rb");
    check(r != NULL, "visible open");
    fseek(r, 0, SEEK_END);
    long n = ftell(r);
    fclose(r);
    return n;
}

int main(void) {
    static char b0[4096], b1[4096], b2[4096];
    const char *names[3] = {"fl_a.txt", "fl_b.txt", "fl_c.txt"};
    FILE *f[3];
    char *bufs[3] = {b0, b1, b2};
    for (int i = 0; i < 3; i++) {
        f[i] = fopen(names[i], "w");
        check(f[i] != NULL, "open");
        check(setvbuf(f[i], bufs[i], _IOFBF, 4096) == 0, "setvbuf");
    }
    for (int i = 0; i < 3; i++)
        for (int k = 0; k <= i; k++)
            fprintf(f[i], "stream %d line %d\n", i, k);

    printf("before flush:");
    for (int i = 0; i < 3; i++)
        printf(" %ld", visible(names[i]));
    printf("\n");

    check(fflush(NULL) == 0, "fflush(NULL)");
    printf("after fflush(NULL):");
    long v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = visible(names[i]);
        printf(" %ld", v[i]);
    }
    printf("\n");
    check(v[0] == 16 && v[1] == 32 && v[2] == 48, "sizes");

    /* flush one stream only */
    fputs("more\n", f[0]);
    fputs("more\n", f[1]);
    check(fflush(f[1]) == 0, "flush one");
    printf("flush b only: %ld %ld\n", visible(names[0]), visible(names[1]));
    check(visible(names[0]) == 16 && visible(names[1]) == 37, "flush one sizes");
    for (int i = 0; i < 3; i++)
        fclose(f[i]);
    printf("after close: %ld\n", visible(names[0]));

    /* r+ stream alternating writes and reads, using fflush to switch direction */
    FILE *g = fopen("fl_rw.txt", "w+");
    check(g != NULL, "rw open");
    fputs("line one\nline two\nline three\n", g);
    fflush(g);
    rewind(g);
    char buf[64];
    check(fgets(buf, sizeof buf, g) != NULL, "fgets");
    long mid = ftell(g);
    fseek(g, mid, SEEK_SET); /* required between read and write */
    fputs("LINE TWO\n", g);
    fflush(g); /* required between write and read */
    check(fgets(buf, sizeof buf, g) != NULL, "fgets");
    buf[strcspn(buf, "\n")] = 0;
    printf("next after patch: \"%s\"\n", buf);
    check(strcmp(buf, "line three") == 0, "line three");
    rewind(g);
    int n = 0;
    while (fgets(buf, sizeof buf, g)) {
        buf[strcspn(buf, "\n")] = 0;
        printf("  %d: %s\n", ++n, buf);
    }
    fclose(g);

    /* fflush on an input stream that has been repositioned is harmless after fseek */
    g = fopen("fl_rw.txt", "r");
    fgetc(g);
    check(fseek(g, 0, SEEK_SET) == 0, "seek");
    check(fflush(g) == 0, "flush input");
    printf("first char after flush: %c\n", fgetc(g));
    fclose(g);

    for (int i = 0; i < 3; i++)
        remove(names[i]);
    remove("fl_rw.txt");
    return 0;
}
