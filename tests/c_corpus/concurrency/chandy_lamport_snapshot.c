/*
 * title: Chandy-Lamport distributed snapshot
 * topic: concurrency
 * covers: global snapshot, markers on FIFO channels, channel recording, conservation invariant
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NP = 5, SENDS = 60, SNAP_AT = 17, START_BAL = 1000, DATA = 0, MARKER = 1 };

typedef struct Msg Msg;
struct Msg {
    int type, from;
    long amt;
    Msg *next;
};
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Msg *head, *tail;
} Box;

static Box box[NP];

typedef struct {
    int to;
    long amt;
} Plan;
static Plan plan[NP][SENDS];
static int expect_data[NP];

typedef struct {
    int id;
    long balance;
    int recorded;
    long rec_balance;
    int marker_from[NP];
    long chan_rec[NP]; /* money in transit on channel from -> me at the time of the cut */
    int markers, data_got;
    long sent_total, recv_total;
} Proc;

static Proc pr[NP];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void post(int to, int type, int from, long amt) {
    Msg *m = malloc(sizeof *m);
    check(m != NULL, "malloc");
    *m = (Msg){type, from, amt, NULL};
    pthread_mutex_lock(&box[to].mu);
    if (box[to].tail)
        box[to].tail->next = m;
    else
        box[to].head = m;
    box[to].tail = m;
    pthread_cond_signal(&box[to].cv);
    pthread_mutex_unlock(&box[to].mu);
}

static Msg *fetch(int me, int block) {
    pthread_mutex_lock(&box[me].mu);
    while (block && !box[me].head)
        pthread_cond_wait(&box[me].cv, &box[me].mu);
    Msg *m = box[me].head;
    if (m) {
        box[me].head = m->next;
        if (!box[me].head)
            box[me].tail = NULL;
    }
    pthread_mutex_unlock(&box[me].mu);
    return m;
}

static void record_state(Proc *p) {
    p->recorded = 1;
    p->rec_balance = p->balance;
    for (int j = 0; j < NP; j++)
        if (j != p->id)
            post(j, MARKER, p->id, 0);
}

static void handle(Proc *p, Msg *m) {
    if (m->type == DATA) {
        p->balance += m->amt;
        p->recv_total += m->amt;
        p->data_got++;
        if (p->recorded && !p->marker_from[m->from])
            p->chan_rec[m->from] += m->amt; /* message was in flight across the cut */
    } else {
        p->markers++;
        p->marker_from[m->from] = 1;
        if (!p->recorded)
            record_state(p); /* the channel the first marker arrived on is recorded as empty */
    }
    free(m);
}

static void *process(void *arg) {
    Proc *p = arg;
    for (int k = 0; k < SENDS; k++) {
        if (p->id == 0 && k == SNAP_AT)
            record_state(p);
        p->balance -= plan[p->id][k].amt;
        p->sent_total += plan[p->id][k].amt;
        post(plan[p->id][k].to, DATA, p->id, plan[p->id][k].amt);
        Msg *m;
        while ((m = fetch(p->id, 0)) != NULL)
            handle(p, m);
    }
    while (p->data_got < expect_data[p->id] || p->markers < NP - 1 || !p->recorded)
        handle(p, fetch(p->id, 1));
    return NULL;
}

int main(void) {
    unsigned s = 0xBEEF1234u;
    for (int i = 0; i < NP; i++)
        for (int k = 0; k < SENDS; k++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            int o = (int)(s % (NP - 1));
            plan[i][k].to = o >= i ? o + 1 : o;
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            plan[i][k].amt = (long)(s % 50) + 1;
            expect_data[plan[i][k].to]++;
        }
    pthread_t th[NP];
    for (int i = 0; i < NP; i++) {
        pthread_mutex_init(&box[i].mu, NULL);
        pthread_cond_init(&box[i].cv, NULL);
        pr[i].id = i;
        pr[i].balance = START_BAL;
    }
    for (int i = 0; i < NP; i++)
        check(pthread_create(&th[i], NULL, process, &pr[i]) == 0, "create");
    for (int i = 0; i < NP; i++)
        pthread_join(th[i], NULL);

    long snap_states = 0, snap_channels = 0, final_total = 0;
    long expect_bal[NP];
    for (int i = 0; i < NP; i++)
        expect_bal[i] = START_BAL;
    for (int i = 0; i < NP; i++)
        for (int k = 0; k < SENDS; k++) {
            expect_bal[i] -= plan[i][k].amt;
            expect_bal[plan[i][k].to] += plan[i][k].amt;
        }
    for (int i = 0; i < NP; i++) {
        check(pr[i].markers == NP - 1, "one marker per incoming channel");
        check(pr[i].balance == expect_bal[i], "final balance");
        check(pr[i].recorded, "recorded");
        snap_states += pr[i].rec_balance;
        for (int j = 0; j < NP; j++)
            snap_channels += pr[i].chan_rec[j];
        final_total += pr[i].balance;
        printf("process %d: final balance %ld, sent %ld, received %ld, markers %d\n", i, pr[i].balance, pr[i].sent_total, pr[i].recv_total, pr[i].markers);
    }
    printf("total at end: %ld\n", final_total);
    check(final_total == (long)NP * START_BAL, "conservation at end");
    /* the recorded global state is consistent: process states plus money in transit equal the total */
    check(snap_channels >= 0, "non-negative channel contents");
    check(snap_states + snap_channels == (long)NP * START_BAL, "snapshot conserves money");
    printf("snapshot consistent: recorded states + in-transit money = %d\n", NP * START_BAL);
    for (int i = 0; i < NP; i++) {
        check(box[i].head == NULL, "mailbox drained");
        pthread_mutex_destroy(&box[i].mu);
        pthread_cond_destroy(&box[i].cv);
    }
    return 0;
}
