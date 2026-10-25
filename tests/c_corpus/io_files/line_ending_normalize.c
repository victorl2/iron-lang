/*
 * title: Line ending detection and normalization
 * topic: io_files
 * covers: CRLF LF CR handling with fgetc/ungetc, counting each style, normalize to LF and to CRLF, idempotence
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int lf, crlf, cr;
    long bytes;
} Stats;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* stream transform: any of CRLF, CR, LF becomes eol (or LF when eol is NULL) */
static long normalize(FILE *in, FILE *out, const char *eol, Stats *st) {
    long lines = 0;
    int c;
    memset(st, 0, sizeof *st);
    while ((c = fgetc(in)) != EOF) {
        st->bytes++;
        if (c == '\r') {
            int d = fgetc(in);
            if (d == '\n') {
                st->crlf++;
                st->bytes++;
            } else {
                st->cr++;
                if (d != EOF)
                    ungetc(d, in);
            }
            fputs(eol ? eol : "\n", out);
            lines++;
        } else if (c == '\n') {
            st->lf++;
            fputs(eol ? eol : "\n", out);
            lines++;
        } else {
            fputc(c, out);
        }
    }
    return lines;
}

static void write_str(const char *name, const char *s, size_t n) {
    FILE *f = fopen(name, "wb");
    check(f != NULL, "open");
    check(fwrite(s, 1, n, f) == n, "write");
    fclose(f);
}

static long slurp(const char *name, char *buf, size_t cap) {
    FILE *f = fopen(name, "rb");
    check(f != NULL, "slurp open");
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return (long)n;
}

static void esc(const char *s, char *out) {
    for (; *s; s++) {
        if (*s == '\r') {
            *out++ = '\\';
            *out++ = 'r';
        } else if (*s == '\n') {
            *out++ = '\\';
            *out++ = 'n';
        } else
            *out++ = *s;
    }
    *out = 0;
}

int main(void) {
    struct {
        const char *label;
        const char *text;
        size_t len;
    } cases[] = {
        {"unix", "one\ntwo\nthree\n", 14},
        {"dos", "one\r\ntwo\r\nthree\r\n", 17},
        {"oldmac", "one\rtwo\rthree\r", 14},
        {"mixed", "a\r\nb\nc\rd\r\r\ne\n\n", 14},
        {"no-eol", "single line", 11},
        {"cr-at-end", "x\r", 2},
        {"empty", "", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        write_str("in.txt", cases[i].text, cases[i].len);
        FILE *in = fopen("in.txt", "rb");
        FILE *out = fopen("lf.txt", "wb");
        check(in && out, "open pair");
        Stats st;
        long lines = normalize(in, out, NULL, &st);
        fclose(in);
        fclose(out);

        char raw[128], shown[256];
        long n = slurp("lf.txt", raw, sizeof raw);
        esc(raw, shown);
        printf("%-9s lf=%d crlf=%d cr=%d lines=%ld out=%ld \"%s\"\n", cases[i].label, st.lf,
               st.crlf, st.cr, lines, n, shown);
        check(st.bytes == (long)cases[i].len, "byte count");
        check(strchr(raw, '\r') == NULL, "no CR left");

        /* idempotent: normalizing the output again changes nothing */
        in = fopen("lf.txt", "rb");
        out = fopen("lf2.txt", "wb");
        Stats st2;
        normalize(in, out, NULL, &st2);
        fclose(in);
        fclose(out);
        char raw2[128];
        slurp("lf2.txt", raw2, sizeof raw2);
        check(strcmp(raw, raw2) == 0, "idempotent");
        check(st2.cr == 0 && st2.crlf == 0, "clean");

        /* convert to CRLF and back must equal the LF form */
        in = fopen("lf.txt", "rb");
        out = fopen("crlf.txt", "wb");
        normalize(in, out, "\r\n", &st2);
        fclose(in);
        fclose(out);
        in = fopen("crlf.txt", "rb");
        out = fopen("back.txt", "wb");
        normalize(in, out, NULL, &st2);
        fclose(in);
        fclose(out);
        char raw3[128];
        slurp("back.txt", raw3, sizeof raw3);
        check(strcmp(raw, raw3) == 0, "roundtrip via CRLF");
    }
    remove("in.txt");
    remove("lf.txt");
    remove("lf2.txt");
    remove("crlf.txt");
    remove("back.txt");
    return 0;
}
