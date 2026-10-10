/*
 * title: fgets with tiny buffers and line reassembly
 * topic: io_files
 * covers: fgets truncation, detecting partial lines, reassembly into growing string, size 1 and 2 buffers
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

/* read one full line with a fixed-size chunk buffer; returns malloc'd string or NULL at EOF */
static char *read_line(FILE *f, size_t chunk, int *pieces) {
    char *buf = malloc(chunk);
    char *out = NULL;
    size_t len = 0;
    *pieces = 0;
    check(buf != NULL, "malloc");
    while (fgets(buf, (int)chunk, f)) {
        size_t n = strlen(buf);
        char *t = realloc(out, len + n + 1);
        check(t != NULL, "realloc");
        out = t;
        memcpy(out + len, buf, n + 1);
        len += n;
        (*pieces)++;
        if (buf[n - 1] == '\n')
            break;
    }
    free(buf);
    return out;
}

int main(void) {
    const char *text = "short\n"
                       "a somewhat longer line that will not fit\n"
                       "\n"
                       "exactly15chars!\n"
                       "no newline at the end";
    FILE *f = fopen("small.txt", "w");
    check(f != NULL, "open");
    fputs(text, f);
    fclose(f);

    size_t sizes[] = {2, 3, 8, 16, 64};
    char *ref[8];
    int refn = 0;
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        f = fopen("small.txt", "r");
        int n = 0, totalp = 0, p;
        char *l;
        while ((l = read_line(f, sizes[si], &p)) != NULL) {
            if (si == 0) {
                check(refn < 8, "ref");
                ref[refn++] = l;
            } else {
                check(n < refn && strcmp(l, ref[n]) == 0, "same lines for every buffer size");
                free(l);
            }
            totalp += p;
            n++;
        }
        fclose(f);
        printf("buffer %2zu: %d lines in %2d fgets calls\n", sizes[si], n, totalp);
        check(n == 5, "line count");
    }
    for (int i = 0; i < refn; i++) {
        size_t n = strlen(ref[i]);
        int nl = n > 0 && ref[i][n - 1] == '\n';
        if (nl)
            ref[i][n - 1] = 0;
        printf("line %d (%zu) %s: \"%s\"\n", i, n, nl ? "nl" : "eof", ref[i]);
        free(ref[i]);
    }

    /* a buffer of size 1 has room for no characters. Whether fgets returns the buffer or NULL
     * differs between libcs (and sanitizer interceptors), so only check that nothing was read. */
    f = fopen("small.txt", "r");
    char one[1] = {'x'};
    if (fgets(one, 1, f) != NULL && one[0] != 0)
        check(0, "size 1 buffer holds only the NUL");
    int unread = fgetc(f) == 's';
    printf("fgets size 1: first byte still unread: %d\n", unread);
    fclose(f);

    /* a chunk boundary that lands exactly before the newline */
    f = fopen("small.txt", "r");
    char b[6];
    check(fgets(b, sizeof b, f) != NULL, "fgets"); /* "short" without newline: 5 chars fit, newline left */
    printf("boundary: \"%s\" then newline=%d\n", b, fgetc(f) == '\n');
    fclose(f);

    remove("small.txt");
    return 0;
}
