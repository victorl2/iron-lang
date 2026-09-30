/*
 * title: Banker's algorithm with waiting client threads
 * topic: concurrency
 * covers: banker's algorithm, safe state check, safe sequence, monitor granting only safe requests
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { P = 5, R = 3 };

typedef struct {
    int avail[R];
    int max[P][R];
    int alloc[P][R];
} Bank;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Safety algorithm: returns 1 and fills seq when a safe order exists. */
static int is_safe(const Bank *b, int *seq) {
    int work[R], finished[P] = {0}, n = 0;
    memcpy(work, b->avail, sizeof work);
    while (n < P) {
        int progressed = 0;
        for (int p = 0; p < P; p++) {
            if (finished[p])
                continue;
            int ok = 1;
            for (int r = 0; r < R; r++)
                if (b->max[p][r] - b->alloc[p][r] > work[r])
                    ok = 0;
            if (ok) {
                for (int r = 0; r < R; r++)
                    work[r] += b->alloc[p][r];
                finished[p] = 1;
                if (seq)
                    seq[n] = p;
                n++;
                progressed = 1;
            }
        }
        if (!progressed)
            return 0;
    }
    return 1;
}

/* 0 = granted, 1 = exceeds claim, 2 = not enough now, 3 = would be unsafe */
static int try_request(Bank *b, int p, const int *req) {
    for (int r = 0; r < R; r++)
        if (req[r] > b->max[p][r] - b->alloc[p][r])
            return 1;
    for (int r = 0; r < R; r++)
        if (req[r] > b->avail[r])
            return 2;
    for (int r = 0; r < R; r++) {
        b->avail[r] -= req[r];
        b->alloc[p][r] += req[r];
    }
    if (is_safe(b, NULL))
        return 0;
    for (int r = 0; r < R; r++) { /* roll back the trial allocation */
        b->avail[r] += req[r];
        b->alloc[p][r] -= req[r];
    }
    return 3;
}

/* ---- threaded clients ---- */
static Bank shared;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int unsafe_states_seen; /* checked after every grant under the lock */
static int grants_total;

typedef struct {
    int id, grants;
} Client;

static void *client(void *arg) {
    Client *c = arg;
    int p = c->id;
    /* ask for the remaining need in three slices, then release everything */
    for (int slice = 0; slice < 3; slice++) {
        int req[R];
        pthread_mutex_lock(&mu);
        for (int r = 0; r < R; r++) {
            int need = shared.max[p][r] - shared.alloc[p][r];
            req[r] = slice == 2 ? need : (need + 1) / 2;
        }
        int rc;
        while ((rc = try_request(&shared, p, req)) != 0) {
            check(rc != 1, "request within claim");
            pthread_cond_wait(&cv, &mu);
        }
        if (!is_safe(&shared, NULL))
            unsafe_states_seen++;
        c->grants++;
        grants_total++;
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    for (int r = 0; r < R; r++) {
        shared.avail[r] += shared.alloc[p][r];
        shared.alloc[p][r] = 0;
        shared.max[p][r] = 0;
    }
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    /* textbook instance: 10 A, 5 B, 7 C */
    Bank b = {{3, 3, 2},
              {{7, 5, 3}, {3, 2, 2}, {9, 0, 2}, {2, 2, 2}, {4, 3, 3}},
              {{0, 1, 0}, {2, 0, 0}, {3, 0, 2}, {2, 1, 1}, {0, 0, 2}}};
    int seq[P];
    int safe = is_safe(&b, seq);
    printf("initial state safe=%d, safe sequence:", safe);
    for (int i = 0; i < P; i++)
        printf(" P%d", seq[i]);
    printf("\n");
    check(safe, "textbook state is safe");

    static const char *const RC[4] = {"granted", "exceeds claim", "not enough now", "unsafe"};
    struct {
        int p, req[R];
    } tests[] = {{1, {1, 0, 2}}, {4, {3, 3, 0}}, {0, {0, 2, 0}}, {3, {0, 0, 2}}, {1, {9, 9, 9}}};
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int rc = try_request(&b, tests[i].p, tests[i].req);
        printf("P%d requests (%d,%d,%d): %s\n", tests[i].p, tests[i].req[0], tests[i].req[1], tests[i].req[2], RC[rc]);
    }
    int after = is_safe(&b, seq);
    check(after, "state stays safe");
    printf("state after requests: safe=%d, sequence:", after);
    for (int i = 0; i < P; i++)
        printf(" P%d", seq[i]);
    printf("\n");

    /* threaded run: every client eventually gets its full claim, never in an unsafe state */
    memset(&shared, 0, sizeof shared);
    int total[R] = {10, 5, 7};
    memcpy(shared.avail, total, sizeof total);
    int maxes[P][R] = {{7, 5, 3}, {3, 2, 2}, {9, 0, 2}, {2, 2, 2}, {4, 3, 3}};
    memcpy(shared.max, maxes, sizeof maxes);
    Client cl[P];
    pthread_t th[P];
    for (int p = 0; p < P; p++) {
        cl[p] = (Client){p, 0};
        check(pthread_create(&th[p], NULL, client, &cl[p]) == 0, "create");
    }
    for (int p = 0; p < P; p++)
        pthread_join(th[p], NULL);
    for (int p = 0; p < P; p++)
        check(cl[p].grants == 3, "grants per client");
    printf("clients finished: grants=%d unsafe states seen=%d\n", grants_total, unsafe_states_seen);
    printf("resources returned: (%d,%d,%d)\n", shared.avail[0], shared.avail[1], shared.avail[2]);
    check(unsafe_states_seen == 0, "never unsafe");
    for (int r = 0; r < R; r++)
        check(shared.avail[r] == total[r], "all resources back");
    return 0;
}
