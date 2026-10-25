/*
 * title: H2O molecule builder synchronizing two atom kinds
 * topic: concurrency
 * covers: rendezvous of typed threads, group slots, molecule generations, composition invariant, shuffled arrival
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { MOLS = 40, NH = 2 * MOLS, NO = MOLS };
typedef enum { HYDROGEN, OXYGEN } Kind;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int cur_h, cur_o, cur_mol, formed;
static int comp[MOLS + 1][2];
static long atom_sum[MOLS + 1];
static int joined_mol[NH + NO];

typedef struct {
    Kind kind;
    int atom_id;
} Atom;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void *atom_thread(void *arg) {
    Atom *a = arg;
    pthread_mutex_lock(&mu);
    /* wait for a free slot of my kind in the molecule being assembled */
    while ((a->kind == HYDROGEN && cur_h == 2) || (a->kind == OXYGEN && cur_o == 1))
        pthread_cond_wait(&cv, &mu);
    int my = cur_mol;
    if (a->kind == HYDROGEN)
        cur_h++;
    else
        cur_o++;
    comp[my][a->kind]++;
    atom_sum[my] += a->atom_id;
    joined_mol[a->atom_id] = my;
    if (cur_h + cur_o == 3) {
        /* molecule complete: bond, then open the slots for the next one */
        formed++;
        cur_mol++;
        cur_h = cur_o = 0;
        pthread_cond_broadcast(&cv);
    }
    while (formed <= my)
        pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    static Atom atoms[NH + NO];
    int order[NH + NO];
    for (int i = 0; i < NH + NO; i++) {
        atoms[i].atom_id = i;
        atoms[i].kind = i < NH ? HYDROGEN : OXYGEN;
        order[i] = i;
    }
    unsigned s = 4242u;
    for (int i = NH + NO - 1; i > 0; i--) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int j = (int)(s % (unsigned)(i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    pthread_t th[NH + NO];
    for (int i = 0; i < NH + NO; i++)
        check(pthread_create(&th[i], NULL, atom_thread, &atoms[order[i]]) == 0, "create");
    for (int i = 0; i < NH + NO; i++)
        pthread_join(th[i], NULL);

    check(formed == MOLS && cur_h == 0 && cur_o == 0, "all molecules formed");
    long total_atoms = 0, all_ids = 0;
    for (int m = 0; m < MOLS; m++) {
        check(comp[m][HYDROGEN] == 2 && comp[m][OXYGEN] == 1, "each molecule is H2O");
        total_atoms += comp[m][HYDROGEN] + comp[m][OXYGEN];
        all_ids += atom_sum[m];
    }
    long expect_ids = 0;
    for (int i = 0; i < NH + NO; i++) {
        expect_ids += i;
        check(joined_mol[i] >= 0 && joined_mol[i] < MOLS, "every atom joined a molecule");
    }
    check(all_ids == expect_ids, "each atom used exactly once");
    int h_count[MOLS] = {0};
    for (int i = 0; i < NH + NO; i++)
        if (atoms[i].kind == HYDROGEN)
            h_count[joined_mol[i]]++;
    for (int m = 0; m < MOLS; m++)
        check(h_count[m] == 2, "hydrogen per molecule");
    printf("hydrogen atoms %d, oxygen atoms %d\n", NH, NO);
    printf("molecules formed %d, atoms consumed %ld\n", formed, total_atoms);
    printf("every molecule has composition H2O: yes\n");
    printf("sum of atom ids %ld\n", all_ids);
    return 0;
}
