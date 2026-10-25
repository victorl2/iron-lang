/*
 * title: Expression lexer with fgetc and ungetc
 * topic: io_files
 * covers: fgetc, ungetc pushback, tokenizer, number and identifier scanning, ftell after ungetc
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { T_NUM, T_ID, T_OP, T_END } Kind;

typedef struct {
    Kind kind;
    char text[32];
    long value;
} Tok;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Tok next(FILE *f) {
    Tok t;
    memset(&t, 0, sizeof t);
    int c;
    while ((c = fgetc(f)) != EOF && isspace(c))
        ;
    if (c == EOF) {
        t.kind = T_END;
        return t;
    }
    size_t n = 0;
    if (isdigit(c)) {
        t.kind = T_NUM;
        while (c != EOF && isdigit(c) && n < sizeof t.text - 1) {
            t.text[n++] = (char)c;
            t.value = t.value * 10 + (c - '0');
            c = fgetc(f);
        }
        if (c != EOF)
            ungetc(c, f);
    } else if (isalpha(c) || c == '_') {
        t.kind = T_ID;
        while (c != EOF && (isalnum(c) || c == '_') && n < sizeof t.text - 1) {
            t.text[n++] = (char)c;
            c = fgetc(f);
        }
        if (c != EOF)
            ungetc(c, f);
    } else {
        t.kind = T_OP;
        t.text[n++] = (char)c;
        int d = fgetc(f);
        /* two-character operators */
        if ((c == '<' || c == '>' || c == '=' || c == '!') && d == '=')
            t.text[n++] = (char)d;
        else if (c == '*' && d == '*')
            t.text[n++] = (char)d;
        else if (d != EOF)
            ungetc(d, f);
    }
    t.text[n] = 0;
    return t;
}

static const char *kname(Kind k) {
    return k == T_NUM ? "NUM" : k == T_ID ? "ID" : k == T_OP ? "OP" : "END";
}

int main(void) {
    const char *src = "total_1 = (12 + 345)*x2 <= 9**2 != y\n  foo(7,8)-1";
    FILE *f = fopen("lex.txt", "w+");
    check(f != NULL, "open");
    fputs(src, f);
    rewind(f);

    int counts[4] = {0};
    long numsum = 0;
    for (;;) {
        Tok t = next(f);
        counts[t.kind]++;
        if (t.kind == T_END)
            break;
        if (t.kind == T_NUM) {
            numsum += t.value;
            printf("%-3s %s (%ld)\n", kname(t.kind), t.text, t.value);
        } else {
            printf("%-3s %s\n", kname(t.kind), t.text);
        }
    }
    printf("numbers %d identifiers %d operators %d, sum of numbers %ld\n", counts[T_NUM],
           counts[T_ID], counts[T_OP], numsum);
    check(counts[T_NUM] == 7 && counts[T_ID] == 4 && counts[T_OP] == 12, "counts");

    /* ungetc semantics: pushed back char is read again, position decrements */
    rewind(f);
    int a = fgetc(f);
    long p1 = ftell(f);
    check(ungetc(a, f) == a, "ungetc");
    long p2 = ftell(f);
    int b = fgetc(f);
    printf("ungetc: first '%c' tell %ld, after pushback tell %ld, reread '%c'\n", a, p1, p2, b);
    check(a == b && p2 == p1 - 1, "pushback");

    /* pushing back a different character is allowed and does not alter the file */
    ungetc('#', f);
    int c = fgetc(f);
    int d = fgetc(f);
    printf("substituted '%c' then continues with '%c'\n", c, d);
    fclose(f);
    remove("lex.txt");
    return 0;
}
