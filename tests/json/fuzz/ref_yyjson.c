/* ref_yyjson.c -- yyjson (src/vendor/yyjson, strict RFC 8259 mode) as a
 * reference for difffuzz.py. Reads a batch file (one document per line,
 * hex encoded) and prints "ERR <code>" or "OK <canonical form>" per line,
 * in the format of jcanon.iron: i:<n> for a signed integer, u:<n> for an
 * unsigned one above INT64_MAX, f:%.17g for a double, s:<hex> for strings.
 * usage: ref_yyjson <batch file> */
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_hex(FILE *o, const char *s, size_t n) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        fputc(hx[c >> 4], o);
        fputc(hx[c & 15], o);
    }
}

static void canon(FILE *o, yyjson_val *v) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: fputs("null", o); break;
    case YYJSON_TYPE_BOOL: fputs(yyjson_get_bool(v) ? "true" : "false", o); break;
    case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(v)) {
        case YYJSON_SUBTYPE_SINT: fprintf(o, "i:%lld", (long long)yyjson_get_sint(v)); break;
        case YYJSON_SUBTYPE_UINT: {
            unsigned long long u = (unsigned long long)yyjson_get_uint(v);
            if (u <= 9223372036854775807ULL) fprintf(o, "i:%llu", u);
            else fprintf(o, "u:%llu", u);
            break;
        }
        default: fprintf(o, "f:%.17g", yyjson_get_real(v)); break;
        }
        break;
    case YYJSON_TYPE_STR:
        fputs("s:", o);
        put_hex(o, yyjson_get_str(v), yyjson_get_len(v));
        break;
    case YYJSON_TYPE_ARR: {
        fputc('[', o);
        size_t i, n;
        yyjson_val *x;
        yyjson_arr_foreach(v, i, n, x) {
            if (i) fputc(',', o);
            canon(o, x);
        }
        fputc(']', o);
        break;
    }
    case YYJSON_TYPE_OBJ: {
        fputc('{', o);
        size_t i, n;
        yyjson_val *k, *x;
        yyjson_obj_foreach(v, i, n, k, x) {
            if (i) fputc(',', o);
            fputs("s:", o);
            put_hex(o, yyjson_get_str(k), yyjson_get_len(k));
            fputc(':', o);
            canon(o, x);
        }
        fputc('}', o);
        break;
    }
    default: fputs("?", o);
    }
}

static int unhex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ref_yyjson <batch>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t cap = 1 << 16, len = 0;
    char *line = malloc(cap);
    int c;
    for (;;) {
        len = 0;
        while ((c = fgetc(f)) != EOF && c != '\n') {
            if (len + 1 >= cap) line = realloc(line, cap *= 2);
            line[len++] = (char)c;
        }
        if (len == 0 && c == EOF) break;
        if (len == 0) continue;
        size_t n = 0;
        if (!(len == 1 && line[0] == '-')) {
            for (size_t i = 0; i + 1 < len; i += 2) line[n++] = (char)(unhex(line[i]) * 16 + unhex(line[i + 1]));
        }
        yyjson_read_err err;
        yyjson_doc *d = yyjson_read_opts(line, n, YYJSON_READ_NOFLAG, NULL, &err);
        if (!d) {
            printf("ERR %u\n", err.code);
        } else {
            fputs("OK ", stdout);
            canon(stdout, yyjson_doc_get_root(d));
            fputc('\n', stdout);
            yyjson_doc_free(d);
        }
        if (c == EOF) break;
    }
    free(line);
    fclose(f);
    return 0;
}
