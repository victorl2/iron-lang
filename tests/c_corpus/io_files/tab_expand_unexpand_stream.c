/*
 * title: Tab expansion and unexpansion filters
 * topic: io_files
 * covers: column tracking, tab stops, expand, unexpand of leading blanks, round trip, backspace and CR column handling
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

static void expand(FILE *in, FILE *out, int tabstop) {
    int col = 0, c;
    while ((c = fgetc(in)) != EOF) {
        if (c == '\t') {
            int n = tabstop - col % tabstop;
            for (int i = 0; i < n; i++)
                fputc(' ', out);
            col += n;
        } else if (c == '\n' || c == '\r') {
            fputc(c, out);
            col = 0;
        } else if (c == '\b') {
            fputc(c, out);
            if (col > 0)
                col--;
        } else {
            fputc(c, out);
            col++;
        }
    }
}

/* convert runs of blanks that end on a tab stop into tabs (all blanks, not just leading) */
static void unexpand(FILE *in, FILE *out, int tabstop) {
    int col = 0, blanks = 0, c;
    for (;;) {
        c = fgetc(in);
        if (c == ' ') {
            blanks++;
            col++;
            if (col % tabstop == 0) {
                if (blanks > 1)
                    fputc('\t', out);
                else
                    fputc(' ', out);
                blanks = 0;
            }
            continue;
        }
        for (int i = 0; i < blanks; i++)
            fputc(' ', out);
        blanks = 0;
        if (c == EOF)
            break;
        fputc(c, out);
        if (c == '\n' || c == '\r')
            col = 0;
        else if (c == '\b')
            col = col > 0 ? col - 1 : 0;
        else
            col++;
    }
}

static void put(const char *name, const char *s) {
    FILE *f = fopen(name, "wb");
    check(f != NULL, "put");
    fputs(s, f);
    fclose(f);
}

static void show(const char *label, const char *name) {
    FILE *f = fopen(name, "rb");
    check(f != NULL, "show");
    printf("%s\n", label);
    char line[128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        printf("  |");
        for (const char *p = line; *p; p++) {
            if (*p == '\b')
                printf("^H");
            else if (*p == '\t')
                printf("\\t");
            else
                putchar(*p);
        }
        printf("|\n");
    }
    fclose(f);
}

static void run(const char *tool, const char *src, const char *dst, int ts) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    check(in && out, "run open");
    if (tool[0] == 'e')
        expand(in, out, ts);
    else
        unexpand(in, out, ts);
    check(fclose(out) == 0, "close");
    fclose(in);
}

static long size_of(const char *name) {
    FILE *f = fopen(name, "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int main(void) {
    put("in.txt", "a\tb\tc\n"
                  "\tindented\n"
                  "12345678\tx\n"
                  "1234567\ty\n"
                  "\t\t\tdeep\n"
                  "ab\bc\td\n"
                  "end");
    show("original with tabs shown as-is:", "in.txt");
    printf("original size %ld\n", size_of("in.txt"));

    run("expand", "in.txt", "e8.txt", 8);
    show("expand -t 8:", "e8.txt");
    run("expand", "in.txt", "e4.txt", 4);
    show("expand -t 4:", "e4.txt");
    printf("sizes: t8=%ld t4=%ld\n", size_of("e8.txt"), size_of("e4.txt"));

    /* after expansion no tabs remain */
    FILE *f = fopen("e8.txt", "rb");
    int c, tabs = 0;
    while ((c = fgetc(f)) != EOF)
        tabs += c == '\t';
    fclose(f);
    check(tabs == 0, "no tabs after expand");

    /* unexpand then expand again must reproduce the expanded text */
    int stops[] = {4, 8};
    const char *ex[] = {"e4.txt", "e8.txt"};
    for (int i = 0; i < 2; i++) {
        run("unexpand", ex[i], "u.txt", stops[i]);
        run("expand", "u.txt", "r.txt", stops[i]);
        FILE *a = fopen(ex[i], "rb"), *b = fopen("r.txt", "rb");
        int same = 1;
        int x, y;
        do {
            x = fgetc(a);
            y = fgetc(b);
            if (x != y)
                same = 0;
        } while (x != EOF && y != EOF);
        fclose(a);
        fclose(b);
        printf("tabstop %d: unexpanded size %ld (expanded %ld), roundtrip %s\n", stops[i],
               size_of("u.txt"), size_of(ex[i]), same ? "ok" : "MISMATCH");
        check(same, "roundtrip");
        check(size_of("u.txt") < size_of(ex[i]), "unexpand shrinks");
    }
    show("unexpand -t 8 of the t8 file:", "u.txt");

    remove("in.txt");
    remove("e8.txt");
    remove("e4.txt");
    remove("u.txt");
    remove("r.txt");
    return 0;
}
