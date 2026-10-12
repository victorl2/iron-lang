/*
 * title: Cigarette smokers problem with pushers
 * topic: concurrency
 * covers: cigarette smokers, agent and pusher threads, ingredient semaphores, scoreboard
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int v;
} Sem;

static void sem_make(Sem *s, int v) {
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->v = v;
}
static void sem_p(Sem *s) {
    pthread_mutex_lock(&s->m);
    while (s->v == 0)
        pthread_cond_wait(&s->c, &s->m);
    s->v--;
    pthread_mutex_unlock(&s->m);
}
static void sem_v(Sem *s) {
    pthread_mutex_lock(&s->m);
    s->v++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
}

enum { TOBACCO, PAPER, MATCH, ROUNDS = 90 };
static const char *const ING[3] = {"tobacco", "paper", "matches"};

static Sem agent_sem, ingredient[3], smoker_sem[3], mutex_sem;
static int is_ing[3]; /* scoreboard guarded by mutex_sem */
static volatile int stop_flag;
static pthread_mutex_t stop_mu = PTHREAD_MUTEX_INITIALIZER;
static int stopping(void) {
    pthread_mutex_lock(&stop_mu);
    int s = stop_flag;
    pthread_mutex_unlock(&stop_mu);
    return s;
}

static int plan[ROUNDS]; /* which ingredient the agent leaves out this round (= the smoker who runs) */
static int smoked[3];
static int wrong_smoker;
static int current_round;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *agent(void *arg) {
    (void)arg;
    for (int r = 0; r < ROUNDS; r++) {
        sem_p(&agent_sem);
        current_round = r;
        int missing = plan[r];
        for (int k = 0; k < 3; k++)
            if (k != missing)
                sem_v(&ingredient[k]);
    }
    sem_p(&agent_sem);
    pthread_mutex_lock(&stop_mu);
    stop_flag = 1;
    pthread_mutex_unlock(&stop_mu);
    for (int k = 0; k < 3; k++) {
        sem_v(&ingredient[k]);
        sem_v(&smoker_sem[k]);
    }
    return NULL;
}

/* A pusher turns single ingredients into "the smoker who needs the rest can go" */
static void *pusher(void *arg) {
    int me = (int)(long)arg;
    int a = (me + 1) % 3, b = (me + 2) % 3;
    for (;;) {
        sem_p(&ingredient[me]);
        if (stopping())
            return NULL;
        sem_p(&mutex_sem);
        if (is_ing[a]) {
            is_ing[a] = 0;
            sem_v(&smoker_sem[b]); /* have me and a: smoker b lacks nothing else */
        } else if (is_ing[b]) {
            is_ing[b] = 0;
            sem_v(&smoker_sem[a]);
        } else {
            is_ing[me] = 1;
        }
        sem_v(&mutex_sem);
    }
}

static void *smoker(void *arg) {
    int me = (int)(long)arg; /* smoker me owns ingredient me, needs the other two */
    for (;;) {
        sem_p(&smoker_sem[me]);
        if (stopping())
            return NULL;
        if (plan[current_round] != me)
            wrong_smoker++;
        smoked[me]++;
        sem_v(&agent_sem);
    }
}

int main(void) {
    unsigned s = 987654321u;
    int expect[3] = {0, 0, 0};
    for (int r = 0; r < ROUNDS; r++) {
        s = s * 1664525u + 1013904223u;
        plan[r] = (int)((s >> 16) % 3);
        expect[plan[r]]++;
    }
    sem_make(&agent_sem, 1);
    sem_make(&mutex_sem, 1);
    for (int k = 0; k < 3; k++) {
        sem_make(&ingredient[k], 0);
        sem_make(&smoker_sem[k], 0);
    }
    pthread_t at, pt[3], st[3];
    check(pthread_create(&at, NULL, agent, NULL) == 0, "agent");
    for (long k = 0; k < 3; k++) {
        check(pthread_create(&pt[k], NULL, pusher, (void *)k) == 0, "pusher");
        check(pthread_create(&st[k], NULL, smoker, (void *)k) == 0, "smoker");
    }
    pthread_join(at, NULL);
    for (int k = 0; k < 3; k++) {
        pthread_join(pt[k], NULL);
        pthread_join(st[k], NULL);
    }
    int total = 0;
    for (int k = 0; k < 3; k++) {
        printf("smoker with %s smoked %d times (agent skipped it %d times)\n", ING[k], smoked[k], expect[k]);
        check(smoked[k] == expect[k], "smoker counts");
        total += smoked[k];
    }
    check(wrong_smoker == 0, "right smoker each round");
    printf("rounds=%d cigarettes=%d wrong smoker=%d\n", ROUNDS, total, wrong_smoker);
    return 0;
}
