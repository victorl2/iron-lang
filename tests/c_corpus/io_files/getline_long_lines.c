/*
 * title: getline with very long and empty lines
 * topic: io_files
 * covers: getline buffer growth, reuse, embedded lengths, missing final newline
 * deps: posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rs = 20240607u;
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

int main(void) {
    size_t lens[] = {0, 1, 5, 80, 1000, 4095, 4096, 4097, 20000, 100000, 3, 0, 65536, 7};
    int n = (int)(sizeof lens / sizeof lens[0]);
    unsigned long sums[14];

    FILE *f = fopen("long.txt", "w");
    check(f != NULL, "open w");
    for (int i = 0; i < n; i++) {
        unsigned long s = 0;
        for (size_t k = 0; k < lens[i]; k++) {
            unsigned r = rnd();
            int c = 'a' + (int)(r % 26);
            fputc(c, f);
            s = s * 31 + (unsigned long)c;
        }
        sums[i] = s;
        if (i != n - 1)
            fputc('\n', f); /* last line has no terminator */
    }
    fclose(f);

    f = fopen("long.txt", "r");
    check(f != NULL, "open r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    int idx = 0;
    size_t maxcap = 0;
    size_t total = 0;
    while ((got = getline(&line, &cap, f)) != -1) {
        check(idx < n, "too many lines");
        size_t len = (size_t)got;
        int had_nl = len > 0 && line[len - 1] == '\n';
        if (had_nl)
            len--;
        check(strlen(line) == (size_t)got, "NUL terminated");
        check(len == lens[idx], "length");
        unsigned long s = 0;
        for (size_t k = 0; k < len; k++)
            s = s * 31 + (unsigned long)(unsigned char)line[k];
        check(s == sums[idx], "content hash");
        printf("line %2d: len %6zu newline=%d\n", idx, len, had_nl);
        if (cap > maxcap)
            maxcap = cap;
        check(cap >= (size_t)got + 1, "capacity");
        total += (size_t)got;
        idx++;
    }
    check(idx == n, "line count");
    check(feof(f) && !ferror(f), "ended by eof");
    printf("lines %d, bytes %zu\n", idx, total);
    /* the buffer is reused: it only grows, never per-line reallocated to smaller */
    check(maxcap >= 100001, "buffer grew to fit longest line");
    free(line);
    fclose(f);

    /* getline with a caller-supplied preallocated small buffer */
    f = fopen("long.txt", "r");
    line = malloc(4);
    cap = 4;
    got = getline(&line, &cap, f);
    printf("first line empty: %d, cap still small enough: %d\n", got == 1 && line[0] == '\n',
           cap >= 2);
    got = getline(&line, &cap, f);
    printf("second line: %zd bytes \"%.1s\"\n", got, line);
    free(line);
    fclose(f);

    remove("long.txt");
    return 0;
}
