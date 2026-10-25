/*
 * title: Map-reduce word count over generated text
 * topic: concurrency
 * covers: partitioned input, per-thread hash maps, reduce/merge after join, tokenizer on chunk boundaries
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 4, VOCAB = 12, WORDS = 6000, MAPSZ = 64 };

static const char *vocab[VOCAB] = {"alpha", "beta",  "gamma", "delta",   "epsilon", "zeta",
                                   "eta",   "theta", "iota",  "kappa",   "lambda",  "mu"};

typedef struct {
    char word[12];
    long count;
    int used;
} Slot;

typedef struct {
    const char *text;
    size_t lo, hi; /* byte range; words start inside [lo, hi) */
    Slot map[MAPSZ];
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned hash_str(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void map_add(Slot *m, const char *w, size_t n, long by) {
    char tmp[12];
    check(n < sizeof tmp, "word length");
    memcpy(tmp, w, n);
    tmp[n] = 0;
    unsigned i = hash_str(tmp) % MAPSZ;
    for (;;) {
        if (!m[i].used) {
            m[i].used = 1;
            memcpy(m[i].word, tmp, n + 1);
            m[i].count = by;
            return;
        }
        if (strcmp(m[i].word, tmp) == 0) {
            m[i].count += by;
            return;
        }
        i = (i + 1) % MAPSZ;
    }
}

static void *mapper(void *p) {
    Arg *a = p;
    size_t i = a->lo;
    /* a word belongs to the chunk in which it starts: skip a word that began before lo */
    if (i > 0 && a->text[i - 1] != ' ')
        while (a->text[i] && a->text[i] != ' ')
            i++;
    while (i < a->hi) {
        while (a->text[i] == ' ')
            i++;
        if (i >= a->hi || !a->text[i])
            break;
        size_t s = i;
        while (a->text[i] && a->text[i] != ' ')
            i++;
        map_add(a->map, a->text + s, i - s, 1);
    }
    return NULL;
}

static int cmp_slot(const void *x, const void *y) {
    const Slot *a = x, *b = y;
    if (a->count != b->count)
        return a->count < b->count ? 1 : -1;
    return strcmp(a->word, b->word);
}

int main(void) {
    unsigned s = 987654321u;
    size_t cap = (size_t)WORDS * 8 + 1;
    char *text = malloc(cap);
    check(text != NULL, "malloc");
    size_t len = 0;
    long truth[VOCAB] = {0};
    for (int i = 0; i < WORDS; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        /* skewed distribution: low-index words are more common */
        int w = (int)((s % 100) * (s % 100) / 850u) % VOCAB;
        truth[w]++;
        size_t n = strlen(vocab[w]);
        memcpy(text + len, vocab[w], n);
        len += n;
        text[len++] = ' ';
    }
    text[len] = 0;

    static Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        memset(&args[i], 0, sizeof args[i]);
        args[i].text = text;
        args[i].lo = len * (size_t)i / T;
        args[i].hi = len * (size_t)(i + 1) / T;
        check(pthread_create(&th[i], NULL, mapper, &args[i]) == 0, "create");
    }
    Slot merged[MAPSZ];
    memset(merged, 0, sizeof merged);
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        for (int k = 0; k < MAPSZ; k++)
            if (args[i].map[k].used)
                map_add(merged, args[i].map[k].word, strlen(args[i].map[k].word), args[i].map[k].count);
    }
    Slot out[MAPSZ];
    int n = 0;
    long total = 0;
    for (int k = 0; k < MAPSZ; k++)
        if (merged[k].used) {
            out[n++] = merged[k];
            total += merged[k].count;
        }
    qsort(out, (size_t)n, sizeof out[0], cmp_slot);
    check(total == WORDS, "total words");
    for (int i = 0; i < n; i++) {
        int w;
        for (w = 0; w < VOCAB; w++)
            if (strcmp(vocab[w], out[i].word) == 0)
                break;
        check(w < VOCAB && truth[w] == out[i].count, "count matches truth");
        printf("%-8s %ld\n", out[i].word, out[i].count);
    }
    printf("distinct=%d total=%ld\n", n, total);
    free(text);
    return 0;
}
