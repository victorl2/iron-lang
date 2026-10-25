/*
 * title: Word wrap and justify text from a file
 * topic: io_files
 * covers: fscanf %s word reading, greedy wrapping, full justification with remainder distribution, overlong words, paragraph breaks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXW = 40, MAXLINE = 200 };

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int width;
    int justify;
    FILE *out;
    char words[64][MAXW];
    int nw;
    int total; /* letters in pending words */
    int lines;
    int longest;
} Wrap;

static void flush_line(Wrap *w, int last) {
    if (w->nw == 0)
        return;
    char line[MAXLINE];
    int o = 0;
    int gaps = w->nw - 1;
    if (w->justify && !last && gaps > 0) {
        int spaces = w->width - w->total;
        int each = spaces / gaps, extra = spaces % gaps;
        for (int i = 0; i < w->nw; i++) {
            o += snprintf(line + o, sizeof line - (size_t)o, "%s", w->words[i]);
            if (i < gaps) {
                int n = each + (i < extra ? 1 : 0);
                for (int k = 0; k < n; k++)
                    line[o++] = ' ';
            }
        }
        line[o] = 0;
    } else {
        for (int i = 0; i < w->nw; i++)
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%s", i ? " " : "", w->words[i]);
    }
    fprintf(w->out, "%s\n", line);
    w->lines++;
    if ((int)strlen(line) > w->longest)
        w->longest = (int)strlen(line);
    w->nw = 0;
    w->total = 0;
}

static void add_word(Wrap *w, const char *word) {
    int len = (int)strlen(word);
    if (w->nw > 0 && w->total + len + w->nw > w->width)
        flush_line(w, 0);
    check(w->nw < 64, "too many words");
    snprintf(w->words[w->nw++], MAXW, "%s", word);
    w->total += len;
}

static void wrap_file(const char *in, const char *out, int width, int justify, int *lines, int *longest) {
    FILE *f = fopen(in, "r");
    FILE *o = fopen(out, "w");
    check(f && o, "wrap open");
    Wrap w = {width, justify, o, {{0}}, 0, 0, 0, 0};
    int c;
    char word[MAXW];
    int n = 0, newlines = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\n' || c == '\t') {
            if (n > 0) {
                word[n] = 0;
                add_word(&w, word);
                n = 0;
            }
            if (c == '\n' && ++newlines == 2) { /* blank line: paragraph break */
                flush_line(&w, 1);
                fputc('\n', o);
                w.lines++;
            }
        } else {
            newlines = 0;
            if (n < MAXW - 1)
                word[n++] = (char)c;
        }
    }
    if (n > 0) {
        word[n] = 0;
        add_word(&w, word);
    }
    flush_line(&w, 1);
    fclose(f);
    check(fclose(o) == 0, "wrap close");
    *lines = w.lines;
    *longest = w.longest;
}

int main(void) {
    const char *text =
        "The standard library treats every file as a stream of bytes and leaves the "
        "structure to the program. Reading words one at a time, breaking lines at a "
        "fixed width, and padding gaps evenly is a classic exercise.\n"
        "\n"
        "Some words, like supercalifragilisticexpialidocious, are longer than a narrow "
        "column and must sit alone on their line.\n";
    FILE *f = fopen("prose.txt", "w");
    check(f != NULL, "open");
    fputs(text, f);
    fclose(f);

    int widths[] = {30, 44};
    for (int wi = 0; wi < 2; wi++) {
        for (int just = 0; just < 2; just++) {
            int lines, longest;
            wrap_file("prose.txt", "wrapped.txt", widths[wi], just, &lines, &longest);
            printf("--- width %d, %s: %d lines, longest %d ---\n", widths[wi],
                   just ? "justified" : "ragged", lines, longest);
            if (wi == 0) {
                FILE *r = fopen("wrapped.txt", "r");
                char line[256];
                while (fgets(line, sizeof line, r)) {
                    line[strcspn(line, "\n")] = 0;
                    printf("|%-*s|\n", widths[wi], line);
                }
                fclose(r);
            }
            /* words are preserved in order */
            FILE *a = fopen("prose.txt", "r"), *b = fopen("wrapped.txt", "r");
            char wa[MAXW], wb[MAXW];
            int nwords = 0, ok = 1;
            for (;;) {
                int ra = fscanf(a, "%39s", wa), rb = fscanf(b, "%39s", wb);
                if (ra != 1 || rb != 1) {
                    ok &= ra == rb;
                    break;
                }
                ok &= strcmp(wa, wb) == 0;
                nwords++;
            }
            fclose(a);
            fclose(b);
            printf("words preserved: %d (%s)\n", nwords, ok ? "same order" : "MISMATCH");
            check(ok, "word order");
            check(longest <= widths[wi] || longest == 35, "width bound");
        }
    }
    remove("prose.txt");
    remove("wrapped.txt");
    return 0;
}
