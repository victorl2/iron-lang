/*
 * title: Bully algorithm leader election
 * topic: concurrency
 * covers: bully election, crashed processes, ELECTION/OK/COORDINATOR messages, perfect failure detector
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N = 8, ELECTION = 0, OK = 1, COORD = 2, START = 3 };
static const char *const KIND[4] = {"ELECTION", "OK", "COORDINATOR", "START"};

typedef struct Msg Msg;
struct Msg {
    int kind, from;
    Msg *next;
};
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Msg *head, *tail;
} Box;

static Box box[N];
static int alive[N];
static int leader_of[N];
static int expect_msgs[N]; /* protocol messages each live process will receive, trigger included */

static pthread_mutex_t stat_mu = PTHREAD_MUTEX_INITIALIZER;
static long sent[4];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void post(int to, int kind, int from) {
    Msg *m = malloc(sizeof *m);
    check(m != NULL, "malloc");
    *m = (Msg){kind, from, NULL};
    if (kind != START) {
        pthread_mutex_lock(&stat_mu);
        sent[kind]++;
        pthread_mutex_unlock(&stat_mu);
    }
    pthread_mutex_lock(&box[to].mu);
    if (box[to].tail)
        box[to].tail->next = m;
    else
        box[to].head = m;
    box[to].tail = m;
    pthread_cond_signal(&box[to].cv);
    pthread_mutex_unlock(&box[to].mu);
}

static Msg *take(int me) {
    pthread_mutex_lock(&box[me].mu);
    while (!box[me].head)
        pthread_cond_wait(&box[me].cv, &box[me].mu);
    Msg *m = box[me].head;
    box[me].head = m->next;
    if (!box[me].head)
        box[me].tail = NULL;
    pthread_mutex_unlock(&box[me].mu);
    return m;
}

static void *process(void *arg) {
    int me = (int)(long)arg;
    int in_election = 0, received = 0;
    while (received < expect_msgs[me]) {
        Msg *m = take(me);
        int kind = m->kind, from = m->from;
        free(m);
        received++;
        if (kind == START || kind == ELECTION) {
            if (kind == ELECTION)
                post(from, OK, me);
            if (!in_election) {
                in_election = 1;
                int higher = 0;
                for (int p = me + 1; p < N; p++)
                    if (alive[p]) {
                        post(p, ELECTION, me);
                        higher++;
                    }
                if (higher == 0) { /* nobody above me is alive: I win */
                    leader_of[me] = me;
                    for (int p = 0; p < me; p++)
                        if (alive[p])
                            post(p, COORD, me);
                }
            }
        } else if (kind == COORD) {
            leader_of[me] = from;
        }
        /* OK: a higher process has taken over, keep waiting for its COORDINATOR */
    }
    return NULL;
}

static void scenario(const char *name, const int *dead, int ndead, int initiator) {
    for (int i = 0; i < N; i++) {
        alive[i] = 1;
        leader_of[i] = -1;
        pthread_mutex_init(&box[i].mu, NULL);
        pthread_cond_init(&box[i].cv, NULL);
        box[i].head = box[i].tail = NULL;
    }
    for (int i = 0; i < ndead; i++)
        alive[dead[i]] = 0;
    memset(sent, 0, sizeof sent);
    int top0 = -1;
    for (int i = 0; i < N; i++)
        if (alive[i])
            top0 = i;
    for (int q = 0; q < N; q++) {
        expect_msgs[q] = 0;
        if (!alive[q])
            continue;
        for (int p = initiator; p < q; p++)
            expect_msgs[q] += alive[p]; /* ELECTION from each lower participant */
        if (q >= initiator) {
            int higher = 0;
            for (int r = q + 1; r < N; r++)
                higher += alive[r];
            expect_msgs[q] += higher; /* OK replies */
        }
        if (q < top0)
            expect_msgs[q] += 1; /* COORDINATOR */
        if (q == initiator)
            expect_msgs[q] += 1; /* START */
    }
    pthread_t th[N];
    for (long i = 0; i < N; i++)
        if (alive[i])
            check(pthread_create(&th[i], NULL, process, (void *)i) == 0, "create");
    check(alive[initiator], "initiator alive");
    post(initiator, START, initiator);
    for (int i = 0; i < N; i++)
        if (alive[i])
            pthread_join(th[i], NULL);

    int top = -1;
    for (int i = 0; i < N; i++)
        if (alive[i])
            top = i;
    printf("%s: dead={", name);
    for (int i = 0; i < ndead; i++)
        printf("%s%d", i ? "," : "", dead[i]);
    printf("} initiator=%d\n", initiator);
    printf("  leader chosen by all live processes: %d\n", top);
    for (int i = 0; i < N; i++)
        if (alive[i])
            check(leader_of[i] == top, "agreement on the highest live id");
    /* predicted traffic: participants are the initiator and every live higher id */
    long pred_el = 0, pred_ok = 0;
    for (int p = initiator; p < N; p++)
        if (alive[p]) {
            int higher = 0;
            for (int q = p + 1; q < N; q++)
                higher += alive[q];
            pred_el += higher;
            pred_ok += higher;
        }
    long pred_coord = 0;
    for (int p = 0; p < top; p++)
        pred_coord += alive[p];
    for (int k = 0; k < 3; k++)
        printf("  %-11s messages: %ld\n", KIND[k], sent[k]);
    check(sent[ELECTION] == pred_el && sent[OK] == pred_ok && sent[COORD] == pred_coord, "message counts");
    for (int i = 0; i < N; i++) {
        pthread_mutex_destroy(&box[i].mu);
        pthread_cond_destroy(&box[i].cv);
    }
}

int main(void) {
    int d1[] = {7};
    scenario("leader crashed", d1, 1, 2);
    int d2[] = {7, 6, 3};
    scenario("two top nodes crashed", d2, 3, 0);
    int d3[] = {7, 6, 5, 4};
    scenario("only low ids left", d3, 4, 1);
    scenario("everyone alive, lowest starts", NULL, 0, 0);
    int d5[] = {1, 3, 5};
    scenario("top alive, odd ids crashed", d5, 3, 6);
    return 0;
}
