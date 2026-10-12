/*
 * title: Strong/weak counts with zombie control blocks
 * topic: memory
 * covers: weak pointers, zombie control block, lock/upgrade, object vs control block lifetime
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int strong;
    int weak; /* weak refs, plus one held collectively by all strong refs */
    void *obj;
    int state; /* 0 alive, 1 expired (zombie) */
} Ctl;

typedef struct { Ctl *c; } Strong;
typedef struct { Ctl *c; } Weak;

typedef struct { int hp; int gold; } Player;

static int live_objs, live_ctls;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Strong st_new(int hp, int gold) {
    Player *p = malloc(sizeof *p);
    Ctl *c = malloc(sizeof *c);
    check(p && c, "alloc");
    p->hp = hp;
    p->gold = gold;
    c->strong = 1;
    c->weak = 1;
    c->obj = p;
    c->state = 0;
    live_objs++;
    live_ctls++;
    Strong s = {c};
    return s;
}

static void ctl_dec_weak(Ctl *c) {
    if (--c->weak == 0) {
        free(c);
        live_ctls--;
    }
}

static void st_drop(Strong *s) {
    Ctl *c = s->c;
    s->c = NULL;
    if (!c)
        return;
    if (--c->strong == 0) {
        free(c->obj);
        c->obj = NULL;
        c->state = 1;
        live_objs--;
        ctl_dec_weak(c);
    }
}

static Strong st_copy(Strong s) {
    check(s.c && s.c->state == 0, "copy of dead strong");
    s.c->strong++;
    return s;
}

static Weak wk_from(Strong s) {
    s.c->weak++;
    Weak w = {s.c};
    return w;
}

static Strong wk_lock(Weak w) {
    Strong s = {NULL};
    if (w.c && w.c->strong > 0) {
        w.c->strong++;
        s.c = w.c;
    }
    return s;
}

static int wk_expired(Weak w) { return w.c->strong == 0; }

static void wk_drop(Weak *w) {
    if (w->c)
        ctl_dec_weak(w->c);
    w->c = NULL;
}

int main(void) {
    Strong a = st_new(100, 5);
    Strong b = st_copy(a);
    Weak w1 = wk_from(a), w2 = wk_from(b), w3 = wk_from(a);
    printf("strong=%d weak=%d\n", a.c->strong, a.c->weak - 1);

    Strong locked = wk_lock(w1);
    check(locked.c != NULL, "lock alive");
    ((Player *)locked.c->obj)->gold += 10;
    printf("gold via lock: %d, strong=%d\n", ((Player *)a.c->obj)->gold, a.c->strong);
    st_drop(&locked);

    st_drop(&a);
    printf("expired after 1 drop: %d\n", wk_expired(w1));
    st_drop(&b);
    printf("expired after 2 drops: %d (objs=%d ctls=%d)\n", wk_expired(w1), live_objs, live_ctls);
    check(live_objs == 0 && live_ctls == 1, "zombie ctl survives");

    Strong again = wk_lock(w2);
    printf("lock on expired: %s\n", again.c ? "got object" : "null");
    check(again.c == NULL, "lock must fail");

    wk_drop(&w1);
    printf("ctls after w1: %d\n", live_ctls);
    wk_drop(&w2);
    printf("ctls after w2: %d\n", live_ctls);
    check(live_ctls == 1, "still one weak");
    wk_drop(&w3);
    printf("ctls after w3: %d\n", live_ctls);
    check(live_ctls == 0, "zombie freed");

    /* observer table: weak refs to many players, kill every third */
    enum { N = 9 };
    Strong owners[N];
    Weak obs[N];
    for (int i = 0; i < N; i++) {
        owners[i] = st_new(10 * i, i);
        obs[i] = wk_from(owners[i]);
    }
    for (int i = 0; i < N; i += 3)
        st_drop(&owners[i]);
    int total = 0;
    for (int i = 0; i < N; i++) {
        Strong s = wk_lock(obs[i]);
        if (s.c) {
            total += ((Player *)s.c->obj)->hp;
            st_drop(&s);
        }
        printf("%c", wk_expired(obs[i]) ? 'x' : 'o');
    }
    printf("\nsurviving hp total: %d, zombies: %d\n", total, live_ctls - (N - 3));
    for (int i = 0; i < N; i++) {
        st_drop(&owners[i]);
        wk_drop(&obs[i]);
    }
    check(live_objs == 0 && live_ctls == 0, "no leaks");
    return 0;
}
