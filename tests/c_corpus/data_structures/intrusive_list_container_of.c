/*
 * title: Intrusive lists with container_of
 * topic: data_structures
 * covers: intrusive list, offsetof, container_of macro, objects on multiple lists, embedded links
 * deps: libc
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

static unsigned long long rs = 0xA54FF53A5F1D36F1ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 20); }

typedef struct Link { struct Link *prev, *next; } Link;
static void lk_init(Link *h) { h->prev = h->next = h; }
static int lk_empty(const Link *h) { return h->next == h; }
static int lk_linked(const Link *n) { return n->next != NULL; }
static void lk_add_tail(Link *h, Link *n) { n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n; }
static void lk_add_head(Link *h, Link *n) { n->next = h->next; n->prev = h; h->next->prev = n; h->next = n; }
static void lk_del(Link *n) { n->prev->next = n->next; n->next->prev = n->prev; n->prev = n->next = NULL; }
static size_t lk_count(const Link *h) { size_t c = 0; for (const Link *p = h->next; p != h; p = p->next) c++; return c; }

/* A task is on the global list, and on exactly one of the state lists. */
typedef enum { S_READY, S_RUNNING, S_DONE, S_COUNT } State;
static const char *state_name[S_COUNT] = { "ready", "running", "done" };

typedef struct {
    int id, cost;
    State state;
    char pad[3];
    Link all;      /* global list */
    Link by_state; /* one of the state lists */
} Task;

#define ALL_TASK(l) container_of(l, Task, all)
#define STATE_TASK(l) container_of(l, Task, by_state)

int main(void) {
    enum { N = 120 };
    Task *tasks = calloc(N, sizeof *tasks); CHECK(tasks);
    Link all, lists[S_COUNT];
    lk_init(&all);
    for (int s = 0; s < S_COUNT; s++) lk_init(&lists[s]);
    for (int i = 0; i < N; i++) {
        Task *t = &tasks[i];
        t->id = i; t->cost = (int)(rnd() % 50) + 1; t->state = S_READY;
        lk_add_tail(&all, &t->all);
        lk_add_tail(&lists[S_READY], &t->by_state);
    }
    /* sanity of the offsetof arithmetic */
    CHECK(ALL_TASK(&tasks[7].all) == &tasks[7]);
    CHECK(STATE_TASK(&tasks[7].by_state) == &tasks[7]);
    CHECK(offsetof(Task, by_state) > offsetof(Task, all));

    long transitions = 0, moves[S_COUNT][S_COUNT] = {{0}};
    for (int step = 0; step < 6000; step++) {
        Task *t = &tasks[rnd() % N];
        State to;
        switch (t->state) {
        case S_READY: to = S_RUNNING; break;
        case S_RUNNING: to = (rnd() % 3) ? S_DONE : S_READY; break;
        default: to = (rnd() % 8 == 0) ? S_READY : S_DONE; break;
        }
        if (to == t->state) continue;
        lk_del(&t->by_state);
        if (rnd() % 2) lk_add_head(&lists[to], &t->by_state); else lk_add_tail(&lists[to], &t->by_state);
        moves[t->state][to]++; t->state = to; transitions++;
        if (step % 500 == 0) { /* occasionally move to the head of the global list */
            lk_del(&t->all); lk_add_head(&all, &t->all);
        }
    }
    size_t total = 0;
    for (int s = 0; s < S_COUNT; s++) {
        size_t c = 0; long cost = 0;
        for (Link *p = lists[s].next; p != &lists[s]; p = p->next) {
            Task *t = STATE_TASK(p); CHECK(t->state == (State)s); c++; cost += t->cost;
            CHECK(p->next->prev == p);
        }
        CHECK(c == lk_count(&lists[s]));
        printf("%-8s count=%-3zu cost=%ld\n", state_name[s], c, cost);
        total += c;
    }
    CHECK(total == N && lk_count(&all) == N);
    size_t seen[N] = {0};
    int first_ids[6], k = 0;
    for (Link *p = all.next; p != &all; p = p->next) { Task *t = ALL_TASK(p); seen[t->id]++; if (k < 6) first_ids[k++] = t->id; }
    for (int i = 0; i < N; i++) CHECK(seen[i] == 1 && lk_linked(&tasks[i].all));
    printf("transitions=%ld\n", transitions);
    for (int a = 0; a < S_COUNT; a++) { for (int b = 0; b < S_COUNT; b++) printf(" %5ld", moves[a][b]); printf("\n"); }
    printf("global head order:");
    for (int i = 0; i < 6; i++) printf(" %d", first_ids[i]);
    printf("\n");
    while (!lk_empty(&all)) lk_del(all.next);
    for (int s = 0; s < S_COUNT; s++) while (!lk_empty(&lists[s])) lk_del(lists[s].next);
    free(tasks);
    return 0;
}
