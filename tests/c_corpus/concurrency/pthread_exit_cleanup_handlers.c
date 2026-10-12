/*
 * title: pthread_exit with cleanup handlers and return values
 * topic: concurrency
 * covers: pthread_exit, pthread_cleanup_push/pop, early exit from nested calls, handler order (LIFO)
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 4 };

typedef struct {
    int id;
    int exit_at;       /* level at which the thread calls pthread_exit; -1 means run to the end */
    char trace[64];    /* records handler invocations */
    int trace_len;
    pthread_mutex_t *mu;
    long *shared_count;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

typedef struct {
    Arg *a;
    char tag;
} Cleanup;

static void note(void *p) {
    Cleanup *c = p;
    c->a->trace[c->a->trace_len++] = c->tag;
}

static void unlock_handler(void *p) {
    pthread_mutex_unlock(p);
}

static void level3(Arg *a) {
    Cleanup c = {a, 'c'};
    pthread_cleanup_push(note, &c);
    if (a->exit_at == 3)
        pthread_exit((void *)(size_t)(a->id * 10 + 3));
    pthread_cleanup_pop(1); /* execute on normal path too */
}

static void level2(Arg *a) {
    Cleanup c = {a, 'b'};
    pthread_cleanup_push(note, &c);
    level3(a);
    if (a->exit_at == 2)
        pthread_exit((void *)(size_t)(a->id * 10 + 2));
    pthread_cleanup_pop(1);
}

static void *worker(void *p) {
    Arg *a = p;
    Cleanup c = {a, 'a'};
    pthread_cleanup_push(note, &c);
    /* the mutex must be released even when the thread leaves via pthread_exit */
    pthread_mutex_lock(a->mu);
    pthread_cleanup_push(unlock_handler, a->mu);
    (*a->shared_count)++;
    level2(a);
    if (a->exit_at == 1)
        pthread_exit((void *)(size_t)(a->id * 10 + 1));
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    a->trace[a->trace_len] = 0;
    return (void *)(size_t)(a->id * 10);
}

int main(void) {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    long count = 0;
    Arg args[T];
    pthread_t th[T];
    int exits[T] = {-1, 1, 2, 3};
    for (int i = 0; i < T; i++) {
        memset(&args[i], 0, sizeof args[i]);
        args[i].id = i + 1;
        args[i].exit_at = exits[i];
        args[i].mu = &mu;
        args[i].shared_count = &count;
        /* run one at a time: each thread holds the mutex until its handlers unlock it */
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
        void *ret;
        pthread_join(th[i], &ret);
        args[i].trace[args[i].trace_len] = 0;
        printf("thread %d exit_at=%d ret=%zu handlers=%s\n", args[i].id, exits[i], (size_t)ret,
               args[i].trace);
        size_t expect_ret = (size_t)(args[i].id * 10 + (exits[i] < 0 ? 0 : exits[i]));
        check((size_t)ret == expect_ret, "return value");
    }
    /* mutex must be free after every path */
    check(pthread_mutex_trylock(&mu) == 0, "mutex released by cleanup handlers");
    pthread_mutex_unlock(&mu);
    check(count == T, "count");
    check(strcmp(args[0].trace, "cba") == 0, "normal path handler order");
    check(strcmp(args[1].trace, "cba") == 0, "exit from worker level, LIFO");
    check(strcmp(args[2].trace, "cba") == 0, "exit from level2, LIFO");
    check(strcmp(args[3].trace, "cba") == 0, "exit from level3, LIFO");
    printf("count=%ld mutex free: yes\n", count);
    pthread_mutex_destroy(&mu);
    return 0;
}
