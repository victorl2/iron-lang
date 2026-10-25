/*
 * title: Run-length file codec with escape bytes
 * topic: io_files
 * covers: binary RLE encoder/decoder over streams, escape marker, run splitting at 255, worst-case expansion, round trip
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ESC = 0x90 };

static unsigned rs = 1618033u;
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

/*
 * Format: runs of 4 or more equal bytes become ESC count byte (count 4..255).
 * A literal ESC byte is written as ESC 0x00 so the decoder can tell them apart.
 */
static void emit_run(FILE *out, int byte, int count) {
    if (byte == ESC) {
        for (int i = 0; i < count; i++) {
            fputc(ESC, out);
            fputc(0, out);
        }
    } else if (count >= 4) {
        fputc(ESC, out);
        fputc(count, out);
        fputc(byte, out);
    } else {
        for (int i = 0; i < count; i++)
            fputc(byte, out);
    }
}

static void encode(FILE *in, FILE *out) {
    int cur = fgetc(in), count = 0;
    if (cur == EOF)
        return;
    count = 1;
    int c;
    while ((c = fgetc(in)) != EOF) {
        if (c == cur && count < 255) {
            count++;
        } else {
            emit_run(out, cur, count);
            cur = c;
            count = 1;
        }
    }
    emit_run(out, cur, count);
}

static int decode(FILE *in, FILE *out) {
    int c;
    while ((c = fgetc(in)) != EOF) {
        if (c != ESC) {
            fputc(c, out);
            continue;
        }
        int n = fgetc(in);
        if (n == EOF)
            return 0;
        if (n == 0) {
            fputc(ESC, out);
            continue;
        }
        int b = fgetc(in);
        if (b == EOF)
            return 0;
        for (int i = 0; i < n; i++)
            fputc(b, out);
    }
    return 1;
}

static long fsize(const char *n) {
    FILE *f = fopen(n, "rb");
    check(f != NULL, "size open");
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fclose(f);
    return s;
}

static int same(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    check(x && y, "same open");
    int r = 1, c, d;
    do {
        c = fgetc(x);
        d = fgetc(y);
        if (c != d)
            r = 0;
    } while (c != EOF && d != EOF);
    fclose(x);
    fclose(y);
    return r;
}

static void generate(const char *name, int kind, size_t n) {
    FILE *f = fopen(name, "wb");
    check(f != NULL, "gen open");
    size_t i = 0;
    while (i < n) {
        unsigned r = rnd();
        int byte, len;
        switch (kind) {
        case 0: /* long runs */
            byte = (int)(r & 0xFF);
            len = 1 + (int)((r >> 8) % 600);
            break;
        case 1: /* no runs: alternating values, worst case */
            byte = (int)(i % 7) * 3 + 1;
            len = 1;
            break;
        case 2: /* lots of escape bytes and short runs */
            byte = (r & 1) ? ESC : (int)(r >> 4 & 0x3);
            len = 1 + (int)((r >> 10) % 5);
            break;
        default: /* all zeros */
            byte = 0;
            len = 1000;
            break;
        }
        for (int k = 0; k < len && i < n; k++, i++)
            fputc(byte, f);
    }
    fclose(f);
}

int main(void) {
    const char *labels[] = {"long runs", "no runs", "escape heavy", "all zeros"};
    for (int kind = 0; kind < 4; kind++) {
        generate("raw.bin", kind, 20000);
        FILE *in = fopen("raw.bin", "rb"), *out = fopen("enc.bin", "wb");
        check(in && out, "enc open");
        encode(in, out);
        fclose(in);
        check(fclose(out) == 0, "enc close");
        in = fopen("enc.bin", "rb");
        out = fopen("dec.bin", "wb");
        check(decode(in, out) == 1, "decode ok");
        fclose(in);
        check(fclose(out) == 0, "dec close");
        long raw = fsize("raw.bin"), enc = fsize("enc.bin");
        int ok = same("raw.bin", "dec.bin");
        printf("%-13s raw %5ld encoded %5ld (%s) roundtrip %s\n", labels[kind], raw, enc,
               enc < raw ? "smaller" : (enc == raw ? "equal" : "larger"), ok ? "ok" : "FAILED");
        check(ok, "roundtrip");
    }

    /* tiny hand-checked cases */
    struct {
        const char *in;
        size_t n;
        const char *hex;
    } cases[] = {
        {"aaaa", 4, "90 04 61"},
        {"aaa", 3, "61 61 61"},
        {"abcccccd", 8, "61 62 90 05 63 64"},
        {"\x90", 1, "90 00"},
        {"\x90\x90", 2, "90 00 90 00"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        FILE *f = fopen("raw.bin", "wb");
        check(fwrite(cases[i].in, 1, cases[i].n, f) == cases[i].n, "case write");
        fclose(f);
        FILE *in = fopen("raw.bin", "rb"), *out = fopen("enc.bin", "wb");
        encode(in, out);
        fclose(in);
        fclose(out);
        f = fopen("enc.bin", "rb");
        char hex[64] = "";
        int c, o = 0;
        while ((c = fgetc(f)) != EOF)
            o += snprintf(hex + o, sizeof hex - (size_t)o, "%s%02x", o ? " " : "", c);
        fclose(f);
        printf("case %zu -> %s\n", i, hex);
        check(strcmp(hex, cases[i].hex) == 0, "hand-checked encoding");
    }

    /* truncated encoded stream is rejected */
    FILE *f = fopen("enc.bin", "wb");
    fputc(ESC, f);
    fclose(f);
    FILE *in = fopen("enc.bin", "rb"), *out = fopen("dec.bin", "wb");
    int ok = decode(in, out);
    fclose(in);
    fclose(out);
    printf("truncated stream accepted: %s\n", ok ? "yes" : "no");
    check(!ok, "truncated rejected");

    remove("raw.bin");
    remove("enc.bin");
    remove("dec.bin");
    return 0;
}
