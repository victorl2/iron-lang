/*
 * title: Hierarchical cancellation tokens with callbacks
 * topic: concurrency
 * covers: cancellation tokens, parent-child propagation, blocking wait for cancel, callback ordering, cooperative search
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Token Token;
struct Token {
    const char *name;
    int cancelled;
    Token *children[4];
    int nchildren;
    const char *cbs[4];
    int ncbs;
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static char log_buf[512];
static int blocked;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void log_add(const char *s) {
    size_t n = strlen(log_buf);
    check(n + strlen(s) + 2 < sizeof log_buf, "log space");
    if (n)
        log_buf[n++] = ' ';
    strcpy(log_buf + n, s);
}

static void token_init(Token *t, const char *name, Token *parent) {
    memset(t, 0, sizeof *t);
    t->name = name;
    if (parent) {
        pthread_mutex_lock(&mu);
        check(parent->nchildren < 4, "children space");
        parent->children[parent->nchildren++] = t;
        t->cancelled = parent->cancelled; /* born cancelled if the parent already is */
        pthread_mutex_unlock(&mu);
    }
}

static void on_cancel(Token *t, const char *tag) {
    pthread_mutex_lock(&mu);
    check(t->ncbs < 4, "cb space");
    t->cbs[t->ncbs++] = tag;
    pthread_mutex_unlock(&mu);
}

static void cancel_locked(Token *t) {
    if (t->cancelled)
        return;
    t->cancelled = 1;
    for (int i = 0; i < t->ncbs; i++)
        log_add(t->cbs[i]);
    for (int i = 0; i < t->nchildren; i++)
        cancel_locked(t->children[i]);
}

static void cancel(Token *t) {
    pthread_mutex_lock(&mu);
    cancel_locked(t);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static int is_cancelled(Token *t) {
    pthread_mutex_lock(&mu);
    int c = t->cancelled;
    pthread_mutex_unlock(&mu);
    return c;
}

static void wait_cancelled(Token *t) {
    pthread_mutex_lock(&mu);
    blocked++;
    pthread_cond_broadcast(&cv);
    while (!t->cancelled)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

/* Scenario 1: a worker cancels its own token after unit 37; siblings are unaffected. */
typedef struct {
    Token *tok;
    int id;
    int units;
} Worker;

static void *unit_worker(void *arg) {
    Worker *w = arg;
    for (int u = 0; u < 200; u++) {
        if (is_cancelled(w->tok))
            break;
        w->units++;
        if (w->id == 1 && u == 37)
            cancel(w->tok);
    }
    return NULL;
}

static void *blocker(void *arg) {
    wait_cancelled((Token *)arg);
    return NULL;
}

/* Scenario 3: search for the first index whose hash is divisible by 401, blocks claimed in order. */
enum { BLOCK = 100, NBLOCKS = 400, SW = 4 };
static int next_block, best = -1;
static Token search_tok;

static int match(int i) {
    unsigned h = (unsigned)i * 2246822519u + 374761393u;
    h ^= h >> 15;
    return h % 401u == 0;
}

static void *searcher(void *arg) {
    for (;;) {
        pthread_mutex_lock(&mu);
        int b = next_block++;
        int skip = b >= NBLOCKS || (best >= 0 && b * BLOCK > best);
        pthread_mutex_unlock(&mu);
        if (skip) {
            cancel(&search_tok);
            return NULL;
        }
        for (int i = b * BLOCK; i < (b + 1) * BLOCK; i++)
            if (match(i)) {
                pthread_mutex_lock(&mu);
                if (best < 0 || i < best)
                    best = i;
                pthread_mutex_unlock(&mu);
                break;
            }
    }
}

int main(void) {
    Token root, c0, c1, c2;
    token_init(&root, "root", NULL);
    token_init(&c0, "c0", &root);
    token_init(&c1, "c1", &root);
    token_init(&c2, "c2", &root);
    Token *kids[3] = {&c0, &c1, &c2};
    Worker w[3];
    pthread_t th[3];
    for (int i = 0; i < 3; i++) {
        w[i].tok = kids[i];
        w[i].id = i;
        w[i].units = 0;
        check(pthread_create(&th[i], NULL, unit_worker, &w[i]) == 0, "create");
    }
    for (int i = 0; i < 3; i++)
        pthread_join(th[i], NULL);
    printf("scenario 1: units done %d %d %d\n", w[0].units, w[1].units, w[2].units);
    printf("scenario 1: root cancelled %d, c1 cancelled %d\n", is_cancelled(&root), is_cancelled(&c1));
    check(w[0].units == 200 && w[1].units == 38 && w[2].units == 200, "self cancel is local");
    check(!is_cancelled(&root), "child does not cancel parent");

    /* Scenario 2: workers block on their tokens until the parent is cancelled. */
    Token r2, d0, d1, d2;
    token_init(&r2, "r2", NULL);
    token_init(&d0, "d0", &r2);
    token_init(&d1, "d1", &r2);
    on_cancel(&r2, "r2.a");
    on_cancel(&r2, "r2.b");
    on_cancel(&d0, "d0.a");
    on_cancel(&d1, "d1.a");
    on_cancel(&d1, "d1.b");
    token_init(&d2, "d2", &r2);
    on_cancel(&d2, "d2.a");
    Token *ds[3] = {&d0, &d1, &d2};
    for (int i = 0; i < 3; i++)
        check(pthread_create(&th[i], NULL, blocker, ds[i]) == 0, "create blocker");
    pthread_mutex_lock(&mu);
    while (blocked < 3)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    check(!is_cancelled(&d0) && !is_cancelled(&d1) && !is_cancelled(&d2), "none cancelled yet");
    cancel(&r2);
    for (int i = 0; i < 3; i++)
        pthread_join(th[i], NULL);
    printf("scenario 2: callback order: %s\n", log_buf);
    check(strcmp(log_buf, "r2.a r2.b d0.a d1.a d1.b d2.a") == 0, "callbacks in registration order, parent first");
    Token late;
    token_init(&late, "late", &r2);
    printf("scenario 2: child created after cancel is cancelled: %d\n", is_cancelled(&late));
    cancel(&r2); /* idempotent */
    check(strlen(log_buf) == strlen("r2.a r2.b d0.a d1.a d1.b d2.a"), "cancel is idempotent");

    /* Scenario 3 */
    token_init(&search_tok, "search", NULL);
    pthread_t st[SW];
    for (int i = 0; i < SW; i++)
        check(pthread_create(&st[i], NULL, searcher, NULL) == 0, "create searcher");
    for (int i = 0; i < SW; i++)
        pthread_join(st[i], NULL);
    int brute = -1;
    for (int i = 0; i < NBLOCKS * BLOCK; i++)
        if (match(i)) {
            brute = i;
            break;
        }
    check(best == brute, "search result equals brute force");
    printf("scenario 3: first match at %d, search token cancelled %d\n", best, is_cancelled(&search_tok));
    return 0;
}
