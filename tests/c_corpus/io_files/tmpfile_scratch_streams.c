/*
 * title: tmpfile scratch runs and external merge
 * topic: io_files
 * covers: tmpfile, one scratch file per sorted run, pairwise merge passes, rewind reuse
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { N = 500, RUN = 64, MAXRUNS = 16 };

static unsigned rs = 424242u;
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

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* merge two sorted scratch streams (both positioned at start) into a new tmpfile */
static FILE *merge2(FILE *a, FILE *b) {
    FILE *dst = tmpfile();
    check(dst != NULL, "tmpfile merge");
    int x = 0, y = 0;
    int ha = fread(&x, sizeof x, 1, a) == 1;
    int hb = fread(&y, sizeof y, 1, b) == 1;
    while (ha || hb) {
        if (ha && (!hb || x <= y)) {
            check(fwrite(&x, sizeof x, 1, dst) == 1, "wa");
            ha = fread(&x, sizeof x, 1, a) == 1;
        } else {
            check(fwrite(&y, sizeof y, 1, dst) == 1, "wb");
            hb = fread(&y, sizeof y, 1, b) == 1;
        }
    }
    rewind(dst);
    return dst;
}

int main(void) {
    int data[N];
    for (int i = 0; i < N; i++)
        data[i] = (int)(rnd() % 1000);

    /* pass 0: each sorted run goes to its own scratch file */
    FILE *runs[MAXRUNS];
    int nruns = 0;
    for (int off = 0; off < N; off += RUN) {
        int n = N - off < RUN ? N - off : RUN;
        int chunk[RUN];
        for (int i = 0; i < n; i++)
            chunk[i] = data[off + i];
        qsort(chunk, (size_t)n, sizeof chunk[0], cmp_int);
        check(nruns < MAXRUNS, "too many runs");
        runs[nruns] = tmpfile();
        check(runs[nruns] != NULL, "tmpfile");
        check(fwrite(chunk, sizeof chunk[0], (size_t)n, runs[nruns]) == (size_t)n, "run write");
        rewind(runs[nruns]);
        nruns++;
    }
    printf("pass 0: %d runs of up to %d ints\n", nruns, RUN);

    /* merge passes: pair up neighbours until one file remains */
    int pass = 0;
    while (nruns > 1) {
        int out = 0;
        for (int i = 0; i + 1 < nruns; i += 2) {
            FILE *a = runs[i], *b = runs[i + 1];
            runs[out++] = merge2(a, b);
            fclose(a);
            fclose(b);
        }
        if (nruns % 2) {
            runs[out] = runs[nruns - 1];
            out++;
        }
        nruns = out;
        pass++;
        printf("pass %d: %d runs left\n", pass, nruns);
    }
    FILE *out = runs[0];

    int expect[N];
    for (int i = 0; i < N; i++)
        expect[i] = data[i];
    qsort(expect, N, sizeof expect[0], cmp_int);
    rewind(out);
    int mism = 0, seen = 0, v;
    long sum = 0;
    while (fread(&v, sizeof v, 1, out) == 1) {
        if (seen >= N || v != expect[seen])
            mism++;
        sum += v;
        seen++;
    }
    printf("merged %d ints, mismatches %d, sum %ld, min %d max %d\n", seen, mism, sum, expect[0],
           expect[N - 1]);
    check(seen == N && mism == 0, "merge correct");

    /* reuse the same scratch file: rewind and overwrite the head with the five largest */
    rewind(out);
    for (int i = 0; i < 5; i++)
        check(fwrite(&expect[N - 1 - i], sizeof(int), 1, out) == 1, "top5");
    fflush(out);
    rewind(out);
    printf("top values:");
    for (int i = 0; i < 5; i++) {
        check(fread(&v, sizeof v, 1, out) == 1, "top read");
        printf(" %d", v);
    }
    printf("\n");
    fseek(out, 0, SEEK_END);
    printf("scratch size still %ld bytes\n", ftell(out));
    check(ftell(out) == N * (long)sizeof(int), "size");
    fclose(out);
    return 0;
}
