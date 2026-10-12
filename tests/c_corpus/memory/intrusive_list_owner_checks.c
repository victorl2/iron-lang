/*
 * title: Intrusive list membership with owner checks
 * topic: memory
 * covers: intrusive doubly linked list, container_of via offsetof, node owner pointer, unlink on destroy, double insert detection
 * deps: libc
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ListHead ListHead;
typedef struct List List;

struct ListHead {
    ListHead *prev, *next;
    List *owner; /* which list currently holds this node, NULL if none */
};

struct List {
    ListHead sentinel;
    const char *name;
    int count;
};

typedef struct {
    int id;
    ListHead by_state; /* membership in exactly one state list */
    ListHead by_age;   /* second, independent membership */
    char label[8];
} Task;

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

static int errors_seen;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void list_init(List *l, const char *name) {
    l->sentinel.prev = l->sentinel.next = &l->sentinel;
    l->sentinel.owner = l;
    l->name = name;
    l->count = 0;
}

static int list_push_back(List *l, ListHead *h) {
    if (h->owner) {
        errors_seen++; /* already in a list: inserting again would corrupt both */
        return -1;
    }
    h->prev = l->sentinel.prev;
    h->next = &l->sentinel;
    l->sentinel.prev->next = h;
    l->sentinel.prev = h;
    h->owner = l;
    l->count++;
    return 0;
}

static int list_remove(ListHead *h) {
    if (!h->owner) {
        errors_seen++;
        return -1;
    }
    h->prev->next = h->next;
    h->next->prev = h->prev;
    h->owner->count--;
    h->owner = NULL;
    h->prev = h->next = NULL;
    return 0;
}

static int list_move(List *to, ListHead *h) {
    if (list_remove(h) != 0)
        return -1;
    return list_push_back(to, h);
}

static Task *task_new(int id, const char *label) {
    Task *t = calloc(1, sizeof *t);
    check(t != NULL, "alloc");
    t->id = id;
    snprintf(t->label, sizeof t->label, "%s", label);
    return t;
}

/* Destroying a task must unlink it from every list it is on. */
static void task_destroy(Task *t) {
    if (t->by_state.owner)
        list_remove(&t->by_state);
    if (t->by_age.owner)
        list_remove(&t->by_age);
    free(t);
}

static void dump(const List *l, size_t off) {
    printf("%-8s(%d):", l->name, l->count);
    for (const ListHead *h = l->sentinel.next; h != &l->sentinel; h = h->next) {
        const Task *t = (const Task *)((const char *)h - off);
        printf(" %s#%d", t->label, t->id);
    }
    printf("\n");
}

int main(void) {
    List ready, running, done, ages;
    list_init(&ready, "ready");
    list_init(&running, "running");
    list_init(&done, "done");
    list_init(&ages, "by-age");

    Task *t[6];
    const char *names[] = {"io", "net", "gc", "ui", "db", "log"};
    for (int i = 0; i < 6; i++) {
        t[i] = task_new(i + 1, names[i]);
        list_push_back(&ready, &t[i]->by_state);
        list_push_back(&ages, &t[i]->by_age);
    }
    dump(&ready, offsetof(Task, by_state));
    dump(&ages, offsetof(Task, by_age));

    printf("double insert: %d\n", list_push_back(&running, &t[0]->by_state));
    printf("cross-list insert: %d\n", list_push_back(&done, &t[1]->by_age));

    for (int i = 0; i < 3; i++)
        list_move(&running, &t[i]->by_state);
    list_move(&done, &t[1]->by_state);
    dump(&ready, offsetof(Task, by_state));
    dump(&running, offsetof(Task, by_state));
    dump(&done, offsetof(Task, by_state));

    /* container_of from a head recovers the task */
    Task *back = container_of(running.sentinel.next, Task, by_state);
    check(back == t[0], "container_of");
    printf("first running: %s\n", back->label);

    /* destroy while linked in two lists */
    task_destroy(t[2]);
    task_destroy(t[4]);
    dump(&running, offsetof(Task, by_state));
    dump(&ready, offsetof(Task, by_state));
    dump(&ages, offsetof(Task, by_age));
    printf("remove unlinked: %d\n", list_remove(&t[0]->by_age) + list_remove(&t[0]->by_age));

    for (int i = 0; i < 6; i++)
        if (i != 2 && i != 4)
            task_destroy(t[i]);
    check(ready.count == 0 && running.count == 0 && done.count == 0 && ages.count == 0,
          "lists empty after destroy");
    check(ready.sentinel.next == &ready.sentinel, "sentinel self-linked");
    printf("errors seen: %d\n", errors_seen);
    return 0;
}
