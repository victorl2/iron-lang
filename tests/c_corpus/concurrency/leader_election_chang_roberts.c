/*
 * title: Chang-Roberts ring leader election
 * topic: concurrency
 * covers: leader election, unidirectional ring, message complexity, per-node forward counts, mailboxes
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXN = 24, ELECT = 0, ELECTED = 1 };

typedef struct Msg Msg;
struct Msg {
    int kind, id;
    Msg *next;
};
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Msg *head, *tail;
} Box;

static Box box[MAXN];
static int N;
static int uid[MAXN];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void post(int to, int kind, int id) {
    Msg *m = malloc(sizeof *m);
    check(m != NULL, "malloc");
    *m = (Msg){kind, id, NULL};
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

typedef struct {
    int pos;
    int leader_seen;
    int is_leader;
    long forwards, swallowed, sent_elect;
} Node;

static void *node(void *arg) {
    Node *nd = arg;
    int me = nd->pos, right = (me + 1) % N;
    post(right, ELECT, uid[me]);
    nd->sent_elect++;
    for (;;) {
        Msg *m = take(me);
        if (m->kind == ELECT) {
            if (m->id > uid[me]) {
                post(right, ELECT, m->id);
                nd->forwards++;
            } else if (m->id < uid[me]) {
                nd->swallowed++;
            } else { /* my own id came all the way round: I am the leader */
                nd->is_leader = 1;
                nd->leader_seen = uid[me];
                post(right, ELECTED, uid[me]);
                free(m);
                /* wait for the announcement to come back around */
                Msg *back = take(me);
                check(back->kind == ELECTED && back->id == uid[me], "announcement returns");
                free(back);
                return NULL;
            }
        } else {
            nd->leader_seen = m->id;
            if (uid[me] != m->id)
                post(right, ELECTED, m->id);
            free(m);
            return NULL;
        }
        free(m);
    }
}

static void run_ring(int n, unsigned seed) {
    N = n;
    /* distinct ids from a shuffled range */
    int pool[MAXN * 4];
    for (int i = 0; i < MAXN * 4; i++)
        pool[i] = 100 + i;
    unsigned s = seed;
    for (int i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        int j = i + (int)((s >> 10) % (unsigned)(MAXN * 4 - i));
        int t = pool[i];
        pool[i] = pool[j];
        pool[j] = t;
        uid[i] = pool[i];
    }
    for (int i = 0; i < n; i++) {
        pthread_mutex_init(&box[i].mu, NULL);
        pthread_cond_init(&box[i].cv, NULL);
        box[i].head = box[i].tail = NULL;
    }
    Node nodes[MAXN];
    pthread_t th[MAXN];
    memset(nodes, 0, sizeof nodes);
    for (int i = 0; i < n; i++) {
        nodes[i].pos = i;
        check(pthread_create(&th[i], NULL, node, &nodes[i]) == 0, "create");
    }
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);

    int maxid = 0, maxpos = -1;
    for (int i = 0; i < n; i++)
        if (uid[i] > maxid) {
            maxid = uid[i];
            maxpos = i;
        }
    /* predicted traffic: the id at i travels until a larger id is met */
    long predicted = 0, total_forwards = 0, total_swallowed = 0;
    for (int i = 0; i < n; i++) {
        int hops = 0;
        int j = i;
        for (;;) {
            j = (j + 1) % n;
            hops++;
            if (j == i || uid[j] > uid[i])
                break;
        }
        predicted += hops;
    }
    int leaders = 0;
    for (int i = 0; i < n; i++) {
        total_forwards += nodes[i].forwards;
        total_swallowed += nodes[i].swallowed;
        leaders += nodes[i].is_leader;
        check(nodes[i].leader_seen == maxid, "everyone knows the leader");
    }
    check(leaders == 1 && nodes[maxpos].is_leader, "unique leader is the maximum id");
    /* ELECT messages sent = n initial sends + forwards; each ends either swallowed or at its owner */
    long elect_msgs = n + total_forwards;
    check(elect_msgs == predicted, "predicted message count");
    check(total_swallowed + 1 == n, "all but the max id are swallowed");
    printf("ring of %2d: ids", n);
    for (int i = 0; i < n && i < 8; i++)
        printf(" %d", uid[i]);
    printf("%s\n", n > 8 ? " ..." : "");
    printf("  leader id %d at position %d, election messages=%ld, announcements=%d\n", maxid, maxpos, elect_msgs, n);
    for (int i = 0; i < n; i++) {
        pthread_mutex_destroy(&box[i].mu);
        pthread_cond_destroy(&box[i].cv);
    }
}

int main(void) {
    run_ring(2, 11u);
    run_ring(7, 22u);
    run_ring(13, 33u);
    run_ring(24, 44u);
    /* extreme layouts: ids ascending clockwise is the best case (2n-1), descending the worst (n(n+1)/2) */
    for (int dir = 0; dir < 2; dir++) {
        int n = 16;
        N = n;
        for (int i = 0; i < n; i++) {
            pthread_mutex_init(&box[i].mu, NULL);
            pthread_cond_init(&box[i].cv, NULL);
            box[i].head = box[i].tail = NULL;
            uid[i] = dir == 0 ? 10 + i : 10 + (n - 1 - i);
        }
        Node nodes[MAXN];
        pthread_t th[MAXN];
        memset(nodes, 0, sizeof nodes);
        for (int i = 0; i < n; i++) {
            nodes[i].pos = i;
            check(pthread_create(&th[i], NULL, node, &nodes[i]) == 0, "create");
        }
        for (int i = 0; i < n; i++)
            pthread_join(th[i], NULL);
        long fw = 0;
        for (int i = 0; i < n; i++)
            fw += nodes[i].forwards;
        long msgs = n + fw;
        printf("%s layout of %d: election messages=%ld\n", dir == 0 ? "ascending" : "descending", n, msgs);
        check(msgs == (dir == 0 ? 2 * n - 1 : n * (n + 1) / 2), "extreme layout counts");
        for (int i = 0; i < n; i++) {
            pthread_mutex_destroy(&box[i].mu);
            pthread_cond_destroy(&box[i].cv);
        }
    }
    return 0;
}
