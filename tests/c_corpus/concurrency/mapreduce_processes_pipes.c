/*
 * title: Word-count map-reduce with processes and pipes
 * topic: concurrency
 * covers: mapper and reducer processes, hash partitioning, fixed-size records on pipes, EOF propagation, fd hygiene
 * deps: libc, posix
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum { MAPPERS = 4, REDUCERS = 3, WORDS_PER_MAPPER = 700, VOCAB = 14 };

static const char *const WORDS[VOCAB] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf",
                                         "hotel", "india", "juliet", "kilo", "lima", "mike", "november"};

typedef struct {
    char word[16];
} Rec; /* map output: one occurrence; 16 bytes so pipe writes are atomic */

typedef struct {
    char word[16];
    long count;
} Out; /* reduce output */

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned hash_word(const char *w) {
    unsigned h = 2166136261u;
    for (; *w; w++)
        h = (h ^ (unsigned char)*w) * 16777619u;
    return h;
}

static int pick_word(unsigned *s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    unsigned r = *s;
    /* skewed: low indices are more common */
    unsigned a = r % VOCAB, b = (r >> 8) % VOCAB;
    return (int)(a < b ? a : b);
}

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k <= 0)
            return 0;
        got += (size_t)k;
    }
    return 1;
}

static int cmp_out(const void *a, const void *b) {
    const Out *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return strcmp(x->word, y->word);
}

int main(void) {
    int data[REDUCERS][2], result[REDUCERS][2];
    for (int r = 0; r < REDUCERS; r++) {
        check(pipe(data[r]) == 0, "pipe");
        check(pipe(result[r]) == 0, "pipe");
    }
    fflush(stdout);
    pid_t rpid[REDUCERS], mpid[MAPPERS];
    for (int r = 0; r < REDUCERS; r++) {
        rpid[r] = fork();
        check(rpid[r] >= 0, "fork reducer");
        if (rpid[r] == 0) {
            for (int k = 0; k < REDUCERS; k++) {
                close(data[k][1]);
                close(result[k][0]);
                if (k != r) {
                    close(data[k][0]);
                    close(result[k][1]);
                }
            }
            Out table[VOCAB];
            int n = 0;
            Rec rec;
            while (read_full(data[r][0], &rec, sizeof rec)) {
                int i;
                for (i = 0; i < n; i++)
                    if (strcmp(table[i].word, rec.word) == 0)
                        break;
                if (i == n) {
                    memset(&table[n], 0, sizeof table[n]);
                    memcpy(table[n].word, rec.word, sizeof rec.word);
                    n++;
                }
                table[i].count++;
            }
            for (int i = 0; i < n; i++) {
                ssize_t w = write(result[r][1], &table[i], sizeof table[i]);
                if (w != (ssize_t)sizeof table[i])
                    _exit(2);
            }
            close(data[r][0]);
            close(result[r][1]);
            _exit(0);
        }
    }
    for (int m = 0; m < MAPPERS; m++) {
        mpid[m] = fork();
        check(mpid[m] >= 0, "fork mapper");
        if (mpid[m] == 0) {
            for (int k = 0; k < REDUCERS; k++) {
                close(data[k][0]);
                close(result[k][0]);
                close(result[k][1]);
            }
            unsigned s = 0x1000u + (unsigned)m * 7777u;
            for (int i = 0; i < WORDS_PER_MAPPER; i++) {
                int w = pick_word(&s);
                Rec rec;
                memset(&rec, 0, sizeof rec);
                strcpy(rec.word, WORDS[w]);
                int r = (int)(hash_word(rec.word) % REDUCERS);
                if (write(data[r][1], &rec, sizeof rec) != (ssize_t)sizeof rec)
                    _exit(2);
            }
            for (int k = 0; k < REDUCERS; k++)
                close(data[k][1]);
            _exit(0);
        }
    }
    /* the parent keeps only the result read ends, otherwise reducers never see EOF */
    for (int r = 0; r < REDUCERS; r++) {
        close(data[r][0]);
        close(data[r][1]);
        close(result[r][1]);
    }
    Out all[VOCAB * 2];
    int nall = 0;
    for (int r = 0; r < REDUCERS; r++) {
        Out o;
        while (read_full(result[r][0], &o, sizeof o)) {
            check(nall < VOCAB * 2, "too many results");
            check((int)(hash_word(o.word) % REDUCERS) == r, "word landed on its own reducer");
            all[nall++] = o;
        }
        close(result[r][0]);
    }
    for (int m = 0; m < MAPPERS; m++) {
        int st = 0;
        check(waitpid(mpid[m], &st, 0) == mpid[m] && WIFEXITED(st) && WEXITSTATUS(st) == 0, "mapper exit");
    }
    for (int r = 0; r < REDUCERS; r++) {
        int st = 0;
        check(waitpid(rpid[r], &st, 0) == rpid[r] && WIFEXITED(st) && WEXITSTATUS(st) == 0, "reducer exit");
    }
    qsort(all, (size_t)nall, sizeof all[0], cmp_out);

    /* single-process reference */
    long expect[VOCAB] = {0};
    for (int m = 0; m < MAPPERS; m++) {
        unsigned s = 0x1000u + (unsigned)m * 7777u;
        for (int i = 0; i < WORDS_PER_MAPPER; i++)
            expect[pick_word(&s)]++;
    }
    long total = 0;
    int distinct = 0;
    for (int w = 0; w < VOCAB; w++)
        if (expect[w])
            distinct++;
    check(distinct == nall, "distinct words");
    for (int i = 0; i < nall; i++) {
        int w = -1;
        for (int k = 0; k < VOCAB; k++)
            if (strcmp(WORDS[k], all[i].word) == 0)
                w = k;
        check(w >= 0 && expect[w] == all[i].count, "counts match reference");
        total += all[i].count;
        printf("%-9s %4ld  reducer %d\n", all[i].word, all[i].count, (int)(hash_word(all[i].word) % REDUCERS));
    }
    printf("distinct=%d total=%ld\n", nall, total);
    check(total == (long)MAPPERS * WORDS_PER_MAPPER, "total words");
    return 0;
}
