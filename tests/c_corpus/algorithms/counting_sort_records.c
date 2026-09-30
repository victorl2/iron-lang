/*
 * title: Stable counting sort on records with key extraction
 * topic: algorithms
 * covers: counting sort, prefix sums, stable backward placement, function pointer key extractor, range offsets
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int score; /* -50..49 */
    char grade;
    int seq;
} Rec;

static unsigned st = 3141592u;
static unsigned rng(void) {
    st = st * 1103515245u + 12345u;
    return (st >> 8) & 0xffffffu;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef int (*KeyFn)(const Rec *);

static int key_score(const Rec *r) { return r->score + 50; }
static int key_grade(const Rec *r) { return r->grade - 'A'; }
static int key_tens(const Rec *r) { return (r->score + 50) / 10; }

static void counting_sort(const Rec *in, Rec *out, int n, KeyFn key, int range, int *counts_out) {
    int *cnt = calloc((size_t)range + 1, sizeof(int));
    check(cnt != NULL, "alloc");
    for (int i = 0; i < n; i++) {
        int k = key(&in[i]);
        check(k >= 0 && k < range, "key in range");
        cnt[k + 1]++;
    }
    if (counts_out)
        memcpy(counts_out, cnt + 1, sizeof(int) * (size_t)range);
    for (int k = 0; k < range; k++)
        cnt[k + 1] += cnt[k];
    /* cnt[k] = first output index for key k; place forward for stability */
    for (int i = 0; i < n; i++)
        out[cnt[key(&in[i])]++] = in[i];
    free(cnt);
}

int main(void) {
    enum { N = 400 };
    static Rec in[N], out[N], out2[N], out3[N];
    for (int i = 0; i < N; i++) {
        in[i].score = (int)(rng() % 100) - 50;
        in[i].grade = (char)('A' + rng() % 5);
        in[i].seq = i;
    }
    int gc[5];
    counting_sort(in, out, N, key_grade, 5, gc);
    for (int i = 1; i < N; i++) {
        check(out[i - 1].grade <= out[i].grade, "grade sorted");
        if (out[i - 1].grade == out[i].grade)
            check(out[i - 1].seq < out[i].seq, "grade stable");
    }
    printf("grade histogram:");
    for (int g = 0; g < 5; g++)
        printf(" %c=%d", 'A' + g, gc[g]);
    printf("\n");

    /* Sort by score, then chain a second stable sort by grade => grade major, score minor (LSD). */
    counting_sort(in, out2, N, key_score, 100, NULL);
    counting_sort(out2, out3, N, key_grade, 5, NULL);
    for (int i = 1; i < N; i++) {
        check(out3[i - 1].grade <= out3[i].grade, "chain grade");
        if (out3[i - 1].grade == out3[i].grade) {
            check(out3[i - 1].score <= out3[i].score, "chain score");
            if (out3[i - 1].score == out3[i].score)
                check(out3[i - 1].seq < out3[i].seq, "chain stable");
        }
    }
    printf("grade then score, first 8:");
    for (int i = 0; i < 8; i++)
        printf(" %c%+d", out3[i].grade, out3[i].score);
    printf("\n");

    int tc[10];
    counting_sort(in, out, N, key_tens, 10, tc);
    printf("decile histogram:");
    int total = 0;
    for (int d = 0; d < 10; d++) {
        printf(" %d", tc[d]);
        total += tc[d];
    }
    printf("\n");
    check(total == N, "histogram total");
    printf("lowest score %+d (seq %d), highest %+d (seq %d)\n", out2[0].score, out2[0].seq, out2[N - 1].score,
           out2[N - 1].seq);
    return 0;
}
