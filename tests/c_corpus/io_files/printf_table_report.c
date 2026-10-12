/*
 * title: Formatted table report with field widths
 * topic: io_files
 * covers: printf field width and precision, star widths, left/right alignment, zero pad, sign flags, file round trip
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    int units;
    long cents;
    unsigned flags;
} Row;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void rule(FILE *f, const int *w, int n) {
    for (int i = 0; i < n; i++) {
        fputc('+', f);
        for (int k = 0; k < w[i] + 2; k++)
            fputc('-', f);
    }
    fputs("+\n", f);
}

int main(void) {
    Row rows[] = {{"anvil", 3, 12999, 0x5u},   {"rope (50m)", 12, 4550, 0x0u},
                  {"lantern", 250, 899, 0xFFu}, {"map", 1, 5, 0x10u},
                  {"a name that is far too long", 40, 1234567, 0x3u}};
    int n = (int)(sizeof rows / sizeof rows[0]);
    int w[4] = {12, 5, 11, 6};

    FILE *f = fopen("table.txt", "w");
    check(f != NULL, "open");
    rule(f, w, 4);
    fprintf(f, "| %-*s | %*s | %*s | %*s |\n", w[0], "item", w[1], "units", w[2], "price", w[3],
            "flags");
    rule(f, w, 4);
    long total = 0;
    for (int i = 0; i < n; i++) {
        char price[24];
        snprintf(price, sizeof price, "%ld.%02ld", rows[i].cents / 100, rows[i].cents % 100);
        fprintf(f, "| %-*.*s | %*d | %*s | 0x%0*X |\n", w[0], w[0], rows[i].name, w[1],
                rows[i].units, w[2], price, w[3] - 2, rows[i].flags);
        total += rows[i].cents * rows[i].units;
    }
    rule(f, w, 4);
    fprintf(f, "| %-*s | %*s | %*ld.%02ld | %*s |\n", w[0], "total", w[1], "", w[2] - 3,
            total / 100, total % 100, w[3], "");
    rule(f, w, 4);
    check(fclose(f) == 0, "close");

    f = fopen("table.txt", "r");
    char line[128];
    int lines = 0;
    size_t width = 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\n");
        if (lines == 0)
            width = len;
        check(len == width, "all lines have equal width");
        printf("%s", line);
        lines++;
    }
    fclose(f);
    printf("%d lines, each %zu columns wide\n", lines, width);

    /* assorted conversions: what widths and flags do to the same value */
    int v = 42;
    printf("[%d] [%5d] [%-5d] [%05d] [%+d] [% d] [%.4d]\n", v, v, v, v, v, v, v);
    printf("[%x] [%#x] [%#o] [%08.3f] [%-8.2f] [%+.1f]\n", 255u, 255u, 8u, 3.14159, 2.5, 7.5);
    printf("[%10.3s] [%-10.3s] [%.0s] [%c%c]\n", "abcdef", "abcdef", "gone", 'o', 'k');
    printf("[%5.1f%%] [%e] [%g] [%g]\n", 99.5, 12345.678, 0.0001, 1234567.0);
    printf("[%*d] [%-*d] [%.*f]\n", 6, 7, 6, 7, 2, 3.14159);
    char sbuf[16];
    int would = snprintf(sbuf, sizeof sbuf, "%s-%d", "truncated-output", 12345);
    printf("snprintf wanted %d chars, kept \"%s\"\n", would, sbuf);
    check(would == 22 && strlen(sbuf) == 15, "snprintf truncation");
    check(total == 12999 * 3 + 4550 * 12 + 899 * 250 + 5 + 1234567L * 40, "total");
    remove("table.txt");
    return 0;
}
