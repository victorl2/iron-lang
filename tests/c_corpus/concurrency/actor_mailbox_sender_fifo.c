/*
 * title: Actor mailboxes with per-sender FIFO ordering
 * topic: concurrency
 * covers: actors, mailboxes, per-sender ordering guarantee, FIN markers with counts, message matrix
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NA = 5, MSGS = 300, DATA = 0, FIN = 1 };

typedef struct Msg Msg;
struct Msg {
    int kind, from;
    long seq;     /* per (sender, receiver) sequence number, starts at 0 */
    long payload; /* DATA: value; FIN: number of DATA messages sent to this receiver */
    Msg *next;
};

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Msg *head, *tail;
} Mailbox;

static Mailbox box[NA];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void post(int to, int kind, int from, long seq, long payload) {
    Msg *m = malloc(sizeof *m);
    check(m != NULL, "malloc");
    *m = (Msg){kind, from, seq, payload, NULL};
    Mailbox *b = &box[to];
    pthread_mutex_lock(&b->mu);
    if (b->tail)
        b->tail->next = m;
    else
        b->head = m;
    b->tail = m;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
}

static Msg *take(int me) {
    Mailbox *b = &box[me];
    pthread_mutex_lock(&b->mu);
    while (!b->head)
        pthread_cond_wait(&b->cv, &b->mu);
    Msg *m = b->head;
    b->head = m->next;
    if (!b->head)
        b->tail = NULL;
    pthread_mutex_unlock(&b->mu);
    return m;
}

typedef struct {
    int id;
    unsigned rng;
    long sent[NA];     /* DATA messages sent to each actor */
    long got[NA];      /* DATA messages received from each actor */
    long sum;          /* sum of payloads received */
    long order_errors; /* per-sender sequence violations */
    long late_data;    /* DATA seen after FIN from the same sender */
    long fin_mismatch;
} Actor;

static unsigned next(unsigned *s) {
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void *actor(void *arg) {
    Actor *a = arg;
    long seq[NA] = {0};
    /* actors interleave sending with receiving so mailboxes see genuinely mixed traffic */
    long expect_seq[NA] = {0};
    int fin_seen[NA] = {0}, fins = 0;
    int sent_total = 0, fin_sent = 0;
    while (fins < NA - 1 || !fin_sent) {
        if (sent_total < MSGS) {
            unsigned r1 = next(&a->rng);
            unsigned r2 = next(&a->rng);
            int to = (int)(r1 % (NA - 1));
            if (to >= a->id)
                to++;
            long val = (long)(r2 % 1000);
            post(to, DATA, a->id, seq[to]++, val);
            a->sent[to]++;
            sent_total++;
        } else if (!fin_sent) {
            for (int t = 0; t < NA; t++)
                if (t != a->id)
                    post(t, FIN, a->id, seq[t]++, a->sent[t]);
            fin_sent = 1;
        }
        /* drain whatever is in the mailbox without blocking, then block only when there is nothing left to send */
        for (;;) {
            Mailbox *b = &box[a->id];
            pthread_mutex_lock(&b->mu);
            int have = b->head != NULL;
            pthread_mutex_unlock(&b->mu);
            if (!have && !(fin_sent && fins < NA - 1))
                break;
            Msg *m = take(a->id);
            if (m->seq != expect_seq[m->from])
                a->order_errors++;
            expect_seq[m->from] = m->seq + 1;
            if (m->kind == DATA) {
                if (fin_seen[m->from])
                    a->late_data++;
                a->got[m->from]++;
                a->sum += m->payload;
            } else {
                fin_seen[m->from] = 1;
                fins++;
                if (m->payload != a->got[m->from])
                    a->fin_mismatch++;
            }
            free(m);
            if (fin_sent && fins == NA - 1)
                break;
        }
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < NA; i++) {
        pthread_mutex_init(&box[i].mu, NULL);
        pthread_cond_init(&box[i].cv, NULL);
    }
    Actor ac[NA];
    pthread_t th[NA];
    memset(ac, 0, sizeof ac);
    for (int i = 0; i < NA; i++) {
        ac[i].id = i;
        ac[i].rng = 0xA5A5u + (unsigned)i * 7717u;
        check(pthread_create(&th[i], NULL, actor, &ac[i]) == 0, "create");
    }
    for (int i = 0; i < NA; i++)
        pthread_join(th[i], NULL);

    printf("messages sent (row = sender, column = receiver):\n");
    for (int i = 0; i < NA; i++) {
        for (int j = 0; j < NA; j++)
            printf(" %4ld", ac[i].sent[j]);
        printf("\n");
    }
    long total_sent = 0;
    for (int j = 0; j < NA; j++) {
        long recv = 0;
        for (int i = 0; i < NA; i++) {
            check(ac[j].got[i] == ac[i].sent[j], "delivery counts");
            recv += ac[j].got[i];
        }
        check(ac[j].order_errors == 0, "per-sender FIFO");
        check(ac[j].late_data == 0, "no data after FIN");
        check(ac[j].fin_mismatch == 0, "FIN counts");
        printf("actor %d received %ld messages, payload sum %ld\n", j, recv, ac[j].sum);
        total_sent += recv;
    }
    printf("total delivered=%ld\n", total_sent);
    check(total_sent == (long)NA * MSGS, "total");
    for (int i = 0; i < NA; i++) {
        check(box[i].head == NULL, "mailbox empty");
        pthread_mutex_destroy(&box[i].mu);
        pthread_cond_destroy(&box[i].cv);
    }
    return 0;
}
