/*
 * title: Two-phase commit across participant threads
 * topic: concurrency
 * covers: two-phase commit, coordinator and participants, prepare locks, vote failures, coordinator crash and recovery
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NPART = 3, NACCT = 4, NTXN = 24, CRASH_TXN = 6, RECOVER_AT = 12 };
enum { PREPARE, VOTE, COMMIT, ABORT, ACK, STOP };

typedef struct Msg Msg;
struct Msg {
    int type, txn, from, acct, delta, yes;
    Msg *next;
};
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Msg *head, *tail;
} Box;

static Box part_box[NPART], coord_box;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void post(Box *b, Msg m) {
    Msg *n = malloc(sizeof *n);
    check(n != NULL, "malloc");
    *n = m;
    n->next = NULL;
    pthread_mutex_lock(&b->mu);
    if (b->tail)
        b->tail->next = n;
    else
        b->head = n;
    b->tail = n;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
}

static Msg take(Box *b) {
    pthread_mutex_lock(&b->mu);
    while (!b->head)
        pthread_cond_wait(&b->cv, &b->mu);
    Msg *n = b->head;
    b->head = n->next;
    if (!b->head)
        b->tail = NULL;
    pthread_mutex_unlock(&b->mu);
    Msg m = *n;
    free(n);
    return m;
}

/* a participant that "runs out of disk" for some transactions */
static int disk_full(int part, int txn) {
    unsigned h = (unsigned)txn * 2654435761u + (unsigned)part * 40503u;
    return ((h >> 7) % 9u) == 0;
}

typedef struct {
    int active;
    int txn, delta;
} Pending;

typedef struct {
    int id;
    long balance[NACCT];
    Pending pend[NACCT];
    int log_prepared, log_committed, log_aborted;
} Part;

static void *participant(void *arg) {
    Part *p = arg;
    for (;;) {
        Msg m = take(&part_box[p->id]);
        if (m.type == STOP)
            return NULL;
        if (m.type == PREPARE) {
            int yes = 1;
            if (p->pend[m.acct].active)
                yes = 0; /* account locked by an in-doubt transaction */
            else if (m.delta < 0 && p->balance[m.acct] + m.delta < 0)
                yes = 0;
            else if (disk_full(p->id, m.txn))
                yes = 0;
            if (yes) {
                p->pend[m.acct] = (Pending){1, m.txn, m.delta};
                p->log_prepared++;
            }
            post(&coord_box, (Msg){VOTE, m.txn, p->id, m.acct, 0, yes, NULL});
        } else { /* COMMIT or ABORT */
            for (int a = 0; a < NACCT; a++)
                if (p->pend[a].active && p->pend[a].txn == m.txn) {
                    if (m.type == COMMIT) {
                        p->balance[a] += p->pend[a].delta;
                        p->log_committed++;
                    } else {
                        p->log_aborted++;
                    }
                    p->pend[a].active = 0;
                }
            post(&coord_box, (Msg){ACK, m.txn, p->id, 0, 0, 0, NULL});
        }
    }
}

typedef struct {
    int fp, fa, tp, ta, amt;
} Txn;

static Txn txns[NTXN];
static int decision[NTXN]; /* 1 commit, 0 abort */

static unsigned rs = 31337u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

int main(void) {
    Part parts[NPART];
    pthread_t th[NPART];
    memset(parts, 0, sizeof parts);
    pthread_mutex_init(&coord_box.mu, NULL);
    pthread_cond_init(&coord_box.cv, NULL);
    for (int i = 0; i < NPART; i++) {
        pthread_mutex_init(&part_box[i].mu, NULL);
        pthread_cond_init(&part_box[i].cv, NULL);
        parts[i].id = i;
        for (int a = 0; a < NACCT; a++)
            parts[i].balance[a] = 100;
        check(pthread_create(&th[i], NULL, participant, &parts[i]) == 0, "create");
    }
    for (int t = 0; t < NTXN; t++) {
        unsigned r1 = rnd();
        unsigned r2 = rnd();
        unsigned r3 = rnd();
        unsigned r4 = rnd();
        txns[t].fp = (int)(r1 % NPART);
        txns[t].tp = (int)((r1 / NPART + 1 + r2 % (NPART - 1)) % NPART);
        if (txns[t].tp == txns[t].fp)
            txns[t].tp = (txns[t].fp + 1) % NPART;
        txns[t].fa = (int)(r3 % NACCT);
        txns[t].ta = (int)(r4 % NACCT);
        txns[t].amt = (int)((r2 >> 8) % 90) + 10;
    }

    int crashed_txn_pending = 0;
    for (int t = 0; t < NTXN; t++) {
        Txn *x = &txns[t];
        post(&part_box[x->fp], (Msg){PREPARE, t, 0, x->fa, -x->amt, 0, NULL});
        post(&part_box[x->tp], (Msg){PREPARE, t, 0, x->ta, x->amt, 0, NULL});
        int vote[NPART] = {-1, -1, -1};
        for (int k = 0; k < 2; k++) {
            Msg v = take(&coord_box);
            check(v.type == VOTE && v.txn == t, "vote");
            vote[v.from] = v.yes;
        }
        int commit = vote[x->fp] == 1 && vote[x->tp] == 1;
        decision[t] = commit; /* the decision is logged before anything is sent */
        printf("txn %2d: P%d.%d -> P%d.%d amount %3d: from votes %s, to votes %s -> %s", t, x->fp, x->fa, x->tp, x->ta, x->amt,
               vote[x->fp] ? "YES" : "NO", vote[x->tp] ? "YES" : "NO", commit ? "COMMIT" : "ABORT");
        if (t == CRASH_TXN) {
            printf(" (coordinator crashes before sending the decision)\n");
            crashed_txn_pending = 1;
            continue;
        }
        printf("\n");
        int type = commit ? COMMIT : ABORT;
        post(&part_box[x->fp], (Msg){type, t, 0, 0, 0, 0, NULL});
        post(&part_box[x->tp], (Msg){type, t, 0, 0, 0, 0, NULL});
        for (int k = 0; k < 2; k++) {
            Msg a = take(&coord_box);
            check(a.type == ACK && a.txn == t, "ack");
        }
        if (t == RECOVER_AT && crashed_txn_pending) {
            Txn *c = &txns[CRASH_TXN];
            int ct = decision[CRASH_TXN] ? COMMIT : ABORT;
            printf("recovery: coordinator replays logged %s for txn %d\n", decision[CRASH_TXN] ? "COMMIT" : "ABORT", CRASH_TXN);
            post(&part_box[c->fp], (Msg){ct, CRASH_TXN, 0, 0, 0, 0, NULL});
            post(&part_box[c->tp], (Msg){ct, CRASH_TXN, 0, 0, 0, 0, NULL});
            for (int k = 0; k < 2; k++) {
                Msg a = take(&coord_box);
                check(a.type == ACK && a.txn == CRASH_TXN, "recovery ack");
            }
            crashed_txn_pending = 0;
        }
    }
    check(!crashed_txn_pending, "recovered");
    for (int i = 0; i < NPART; i++) {
        post(&part_box[i], (Msg){STOP, 0, 0, 0, 0, 0, NULL});
        pthread_join(th[i], NULL);
    }

    long total = 0;
    int commits = 0;
    for (int t = 0; t < NTXN; t++)
        commits += decision[t];
    for (int i = 0; i < NPART; i++) {
        printf("P%d balances:", i);
        for (int a = 0; a < NACCT; a++) {
            printf(" %ld", parts[i].balance[a]);
            total += parts[i].balance[a];
            check(!parts[i].pend[a].active, "no in-doubt locks left");
        }
        printf("  prepared=%d committed=%d aborted=%d\n", parts[i].log_prepared, parts[i].log_committed, parts[i].log_aborted);
        check(parts[i].log_prepared == parts[i].log_committed + parts[i].log_aborted, "log consistency");
    }
    printf("committed=%d aborted=%d total=%ld\n", commits, NTXN - commits, total);
    check(total == (long)NPART * NACCT * 100, "money conserved");
    for (int i = 0; i < NPART; i++) {
        pthread_mutex_destroy(&part_box[i].mu);
        pthread_cond_destroy(&part_box[i].cv);
    }
    pthread_mutex_destroy(&coord_box.mu);
    pthread_cond_destroy(&coord_box.cv);
    return 0;
}
