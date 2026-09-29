/*
 * title: Thread-safe logging into a shared buffer
 * topic: concurrency
 * covers: shared append buffer, mutex-guarded snprintf, variadic logger, sorted output after join
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t mu;
    char *data;
    size_t len, cap;
    int lines;
} Log;

static Log lg = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0};

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void log_line(const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp - 1, fmt, ap);
    va_end(ap);
    check(n >= 0 && n < (int)sizeof tmp - 1, "line fits");
    tmp[n++] = '\n';
    pthread_mutex_lock(&lg.mu);
    if (lg.len + (size_t)n + 1 > lg.cap) {
        size_t nc = lg.cap ? lg.cap * 2 : 64;
        while (nc < lg.len + (size_t)n + 1)
            nc *= 2;
        char *nd = realloc(lg.data, nc);
        check(nd != NULL, "realloc");
        lg.data = nd;
        lg.cap = nc;
    }
    memcpy(lg.data + lg.len, tmp, (size_t)n);
    lg.len += (size_t)n;
    lg.data[lg.len] = 0;
    lg.lines++;
    pthread_mutex_unlock(&lg.mu);
}

typedef struct {
    int id;
    int count;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < a->count; i++)
        log_line("worker=%d seq=%03d square=%d tag=%s", a->id, i, i * i, (i % 2) ? "odd" : "even");
    return NULL;
}

static int cmp_line(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void) {
    enum { T = 5 };
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].id = i;
        args[i].count = 40 + i * 5;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);

    int expect = 0;
    for (int i = 0; i < T; i++)
        expect += args[i].count;
    check(lg.lines == expect, "line count");

    /* split into lines, verify each is intact (not interleaved), sort, print a sample */
    char **lines = malloc(sizeof(char *) * (size_t)lg.lines);
    check(lines != NULL, "malloc");
    int n = 0;
    char *save = lg.data;
    for (size_t i = 0; i < lg.len; i++)
        if (lg.data[i] == '\n') {
            lg.data[i] = 0;
            lines[n++] = save;
            save = lg.data + i + 1;
        }
    check(n == lg.lines, "split");
    int per_worker[T] = {0};
    for (int i = 0; i < n; i++) {
        int w, s, sq;
        char tag[8];
        check(sscanf(lines[i], "worker=%d seq=%d square=%d tag=%7s", &w, &s, &sq, tag) == 4,
              "line intact");
        check(w >= 0 && w < T && sq == s * s, "line content");
        check(strcmp(tag, (s % 2) ? "odd" : "even") == 0, "tag");
        per_worker[w]++;
    }
    qsort(lines, (size_t)n, sizeof(char *), cmp_line);
    for (int i = 1; i < n; i++)
        check(strcmp(lines[i - 1], lines[i]) < 0, "unique lines");
    printf("lines=%d bytes=%zu\n", n, lg.len);
    for (int i = 0; i < T; i++)
        printf("worker %d logged %d\n", i, per_worker[i]);
    printf("first: %s\n", lines[0]);
    printf("last: %s\n", lines[n - 1]);
    free(lines);
    free(lg.data);
    return 0;
}
