/*
 * title: Observer list with unsubscribe during notify
 * topic: memory
 * covers: callback lifetimes, tombstones, deferred compaction, re-entrant subscribe/unsubscribe, subscriber lifetime
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Subject Subject;
typedef void (*Callback)(Subject *, void *self, int event);

typedef struct {
    Callback cb;
    void *self;
    unsigned id;
    int dead; /* tombstone: skipped by notify, reclaimed after the outermost notify */
} Sub;

struct Subject {
    Sub *subs;
    int n, cap;
    int notify_depth;
    int tombstones;
    unsigned next_id;
};

typedef struct {
    const char *name;
    unsigned my_id;
    int seen;
    int quit_after; /* unsubscribe self after this many events */
    int spawn_on;   /* subscribe a new child observer on this event value */
    int killer_of;  /* index of observer to unsubscribe on first event, or -1 */
} Obs;

static char trace[512];
static Obs obs[8];
static int nobs;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static unsigned subscribe(Subject *s, Callback cb, void *self) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 2;
        s->subs = realloc(s->subs, (size_t)s->cap * sizeof(Sub));
        check(s->subs != NULL, "realloc");
    }
    Sub *e = &s->subs[s->n++];
    e->cb = cb;
    e->self = self;
    e->id = ++s->next_id;
    e->dead = 0;
    return e->id;
}

static void compact(Subject *s) {
    int w = 0;
    for (int i = 0; i < s->n; i++)
        if (!s->subs[i].dead)
            s->subs[w++] = s->subs[i];
    s->n = w;
    s->tombstones = 0;
}

static int unsubscribe(Subject *s, unsigned id) {
    for (int i = 0; i < s->n; i++)
        if (s->subs[i].id == id && !s->subs[i].dead) {
            if (s->notify_depth > 0) {
                s->subs[i].dead = 1;
                s->tombstones++;
            } else {
                memmove(&s->subs[i], &s->subs[i + 1], (size_t)(s->n - i - 1) * sizeof(Sub));
                s->n--;
            }
            return 1;
        }
    return 0;
}

static void notify(Subject *s, int event) {
    s->notify_depth++;
    int limit = s->n; /* observers added during notify only see later events */
    for (int i = 0; i < limit; i++) {
        Sub cur = s->subs[i]; /* copy: the array may be reallocated by the callback */
        if (cur.dead)
            continue;
        cur.cb(s, cur.self, event);
    }
    if (--s->notify_depth == 0 && s->tombstones)
        compact(s);
}

static void on_event(Subject *s, void *self, int event) {
    Obs *o = self;
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%s:%d ", o->name, event);
    strcat(trace, tmp);
    o->seen++;
    if (o->killer_of >= 0 && o->seen == 1)
        unsubscribe(s, obs[o->killer_of].my_id);
    if (o->quit_after && o->seen == o->quit_after)
        unsubscribe(s, o->my_id);
    if (o->spawn_on == event && nobs < 8) {
        Obs *c = &obs[nobs++];
        c->name = "child";
        c->quit_after = 0;
        c->spawn_on = -1;
        c->killer_of = -1;
        c->my_id = subscribe(s, on_event, c);
    }
}

int main(void) {
    Subject s = {0};
    obs[nobs++] = (Obs){"A", 0, 0, 2, -1, -1};
    obs[nobs++] = (Obs){"B", 0, 0, 0, 2, -1};
    obs[nobs++] = (Obs){"C", 0, 0, 0, -1, 3}; /* on first event kills observer index 3 */
    obs[nobs++] = (Obs){"D", 0, 0, 0, -1, -1};
    for (int i = 0; i < 4; i++)
        obs[i].my_id = subscribe(&s, on_event, &obs[i]);

    for (int ev = 1; ev <= 4; ev++) {
        trace[0] = 0;
        notify(&s, ev);
        printf("event %d: %s| subs=%d tomb=%d\n", ev, trace, s.n, s.tombstones);
        check(s.tombstones == 0 && s.notify_depth == 0, "compacted");
    }
    int total = 0;
    for (int i = 0; i < nobs; i++) {
        printf("%s saw %d\n", obs[i].name, obs[i].seen);
        total += obs[i].seen;
    }
    check(obs[3].seen == 0, "D tombstoned by C before its turn");
    printf("total deliveries: %d\n", total);
    printf("unsubscribe unknown id: %d\n", unsubscribe(&s, 999));
    free(s.subs);
    return 0;
}
