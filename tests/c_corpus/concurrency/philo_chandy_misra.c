/*
 * title: Chandy-Misra dining philosophers (dirty and clean forks)
 * topic: concurrency
 * covers: hygienic solution, request tokens, dirty/clean forks, precedence graph, monitor
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { N = 6, MEALS = 250 };

/*
 * Fork f sits between philosopher f and philosopher (f+1)%N.
 * holder[f]   : who has the fork
 * dirty[f]    : 1 if the fork was used since it was last cleaned
 * token[f]    : who has the request token (always the one WITHOUT the fork unless a
 *               request is pending at the holder)
 * Initially every fork goes to the lower-numbered neighbour, dirty, and the token to the other.
 */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int holder[N], dirty[N], token[N];
static int eating[N], hungry[N];
static long transfers;
static atomic_int in_use[N];

typedef struct {
    int id, meals, bad;
} Phil;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int lo(int f) { return f; }
static int hi(int f) { return (f + 1) % N; }

/* Apply every legal hand-over. Caller holds mu. */
static void settle(void) {
    int moved = 1;
    while (moved) {
        moved = 0;
        for (int f = 0; f < N; f++) {
            int h = holder[f];
            if (token[f] == h && dirty[f] && !eating[h]) {
                int other = h == lo(f) ? hi(f) : lo(f);
                holder[f] = other;
                dirty[f] = 0; /* cleaned before it is sent */
                /* token stays with h: the fork is now at "other" */
                transfers++;
                moved = 1;
            }
        }
    }
    pthread_cond_broadcast(&cv);
}

static void *philosopher(void *arg) {
    Phil *p = arg;
    int id = p->id;
    int fl = (id + N - 1) % N; /* fork shared with left neighbour */
    int fr = id;
    for (int m = 0; m < MEALS; m++) {
        pthread_mutex_lock(&mu);
        hungry[id] = 1;
        for (;;) {
            int fk[2] = {fl, fr};
            for (int k = 0; k < 2; k++) {
                int f = fk[k];
                if (holder[f] != id && token[f] == id) {
                    token[f] = holder[f]; /* send the request */
                }
            }
            settle();
            if (holder[fl] == id && holder[fr] == id)
                break;
            pthread_cond_wait(&cv, &mu);
        }
        eating[id] = 1;
        pthread_mutex_unlock(&mu);

        if (atomic_exchange(&in_use[fl], 1) != 0)
            p->bad++;
        if (atomic_exchange(&in_use[fr], 1) != 0)
            p->bad++;
        p->meals++;
        atomic_store(&in_use[fl], 0);
        atomic_store(&in_use[fr], 0);

        pthread_mutex_lock(&mu);
        eating[id] = 0;
        hungry[id] = 0;
        dirty[fl] = 1;
        dirty[fr] = 1;
        settle();
        pthread_mutex_unlock(&mu);
    }
    /* keep serving neighbours' requests after finishing: settle is run by them */
    return NULL;
}

int main(void) {
    for (int f = 0; f < N; f++) {
        holder[f] = lo(f) < hi(f) ? lo(f) : hi(f);
        token[f] = holder[f] == lo(f) ? hi(f) : lo(f);
        dirty[f] = 1;
    }
    /* initial precedence graph is acyclic: fork N-1 goes to philosopher 0 */
    Phil ph[N] = {{0, 0, 0}};
    pthread_t th[N];
    for (int i = 0; i < N; i++) {
        ph[i].id = i;
        check(pthread_create(&th[i], NULL, philosopher, &ph[i]) == 0, "create");
    }
    for (int i = 0; i < N; i++)
        pthread_join(th[i], NULL);

    long total = 0;
    for (int i = 0; i < N; i++) {
        check(ph[i].bad == 0, "exclusive forks");
        check(ph[i].meals == MEALS, "meals");
        total += ph[i].meals;
    }
    for (int f = 0; f < N; f++)
        printf("initial fork %d: holder=%d token=%d\n", f, f < hi(f) ? lo(f) : hi(f), f < hi(f) ? hi(f) : lo(f));
    printf("philosophers=%d meals each=%d total=%ld\n", N, MEALS, total);
    check(transfers > 0, "forks moved");
    printf("forks changed hands: %s\n", transfers > 0 ? "yes" : "no");
    for (int f = 0; f < N; f++)
        check(holder[f] == lo(f) || holder[f] == hi(f), "holder is a neighbour");
    return 0;
}
