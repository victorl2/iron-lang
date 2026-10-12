/*
 * title: Streaming transforms rot13 and case folding
 * topic: io_files
 * covers: fgetc/fputc filters, function pointer transform table, rot13 involution, upper/lower/swap/title case, punctuation untouched
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*Xform)(int c, int *state);

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int x_rot13(int c, int *st) {
    (void)st;
    if (c >= 'a' && c <= 'z')
        return 'a' + (c - 'a' + 13) % 26;
    if (c >= 'A' && c <= 'Z')
        return 'A' + (c - 'A' + 13) % 26;
    return c;
}

static int x_upper(int c, int *st) {
    (void)st;
    return toupper(c);
}

static int x_lower(int c, int *st) {
    (void)st;
    return tolower(c);
}

static int x_swap(int c, int *st) {
    (void)st;
    return isupper(c) ? tolower(c) : toupper(c);
}

/* title case: first letter of each word upper, rest lower; state = inside word */
static int x_title(int c, int *st) {
    if (isalpha(c)) {
        int r = *st ? tolower(c) : toupper(c);
        *st = 1;
        return r;
    }
    if (c != '\'')
        *st = 0;
    return c;
}

/* rot47 over printable ASCII */
static int x_rot47(int c, int *st) {
    (void)st;
    if (c >= 33 && c <= 126)
        return 33 + (c - 33 + 47) % 94;
    return c;
}

static void filter(const char *src, const char *dst, Xform x) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    check(in && out, "filter open");
    int st = 0, c;
    while ((c = fgetc(in)) != EOF)
        check(fputc(x(c, &st), out) != EOF, "fputc");
    check(fclose(out) == 0, "close");
    fclose(in);
}

static void slurp(const char *name, char *buf, size_t cap) {
    FILE *f = fopen(name, "rb");
    check(f != NULL, "slurp");
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
}

int main(void) {
    const char *text = "Hello, World! It's 2024: the quick brown Fox\n"
                       "jumps over THE lazy dog (again & again).\n"
                       "Path/to-file_name.TXT 100% done?\n";
    FILE *f = fopen("in.txt", "w");
    check(f != NULL, "open");
    fputs(text, f);
    fclose(f);

    struct {
        const char *name;
        Xform fn;
    } table[] = {{"rot13", x_rot13}, {"upper", x_upper}, {"lower", x_lower},
                 {"swap", x_swap},   {"title", x_title}, {"rot47", x_rot47}};
    char out[256];
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        filter("in.txt", "out.txt", table[i].fn);
        slurp("out.txt", out, sizeof out);
        printf("== %s ==\n%s", table[i].name, out);
        check(strlen(out) == strlen(text), "length preserved");
    }

    /* involutions: applying twice restores the input */
    Xform inv[] = {x_rot13, x_rot47, x_swap};
    const char *iname[] = {"rot13", "rot47", "swap"};
    for (int i = 0; i < 3; i++) {
        filter("in.txt", "out.txt", inv[i]);
        filter("out.txt", "back.txt", inv[i]);
        slurp("back.txt", out, sizeof out);
        printf("%s twice restores input: %s\n", iname[i], strcmp(out, text) == 0 ? "yes" : "NO");
        check(strcmp(out, text) == 0, "involution");
    }

    /* upper then lower equals lower; lower is idempotent */
    filter("in.txt", "a.txt", x_upper);
    filter("a.txt", "b.txt", x_lower);
    filter("in.txt", "c.txt", x_lower);
    char b1[256], b2[256];
    slurp("b.txt", b1, sizeof b1);
    slurp("c.txt", b2, sizeof b2);
    check(strcmp(b1, b2) == 0, "upper then lower");
    printf("upper-then-lower equals lower: yes\n");

    /* known rot13 vector */
    f = fopen("v.txt", "w");
    fputs("Why did the chicken cross the road?", f);
    fclose(f);
    filter("v.txt", "v2.txt", x_rot13);
    slurp("v2.txt", out, sizeof out);
    printf("%s\n", out);
    check(strcmp(out, "Jul qvq gur puvpxra pebff gur ebnq?") == 0, "rot13 vector");

    const char *names[] = {"in.txt", "out.txt", "back.txt", "a.txt", "b.txt", "c.txt", "v.txt", "v2.txt"};
    for (int i = 0; i < 8; i++)
        remove(names[i]);
    return 0;
}
