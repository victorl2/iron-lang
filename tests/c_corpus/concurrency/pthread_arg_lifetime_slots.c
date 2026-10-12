/*
 * title: Thread argument lifetime done right
 * topic: concurrency
 * covers: per-thread argument slots, heap-owned args, loop-variable pitfall avoided, string args
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T = 8 };

typedef struct {
    int index;
    char name[16];
    int result;
} Slot;

typedef struct {
    int value;
    char *label; /* heap copy owned by the thread */
    int *out;
} HeapArg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Correct pattern 1: each thread gets its own element of a slot array that outlives the join. */
static void *slot_worker(void *p) {
    Slot *s = p;
    s->result = s->index * 100 + (int)strlen(s->name);
    return NULL;
}

/* Correct pattern 2: heap-allocated argument, freed by the thread that consumes it. */
static void *heap_worker(void *p) {
    HeapArg *h = p;
    *h->out = h->value * 3 + (int)strlen(h->label);
    free(h->label);
    free(h);
    return NULL;
}

/* Correct pattern 3: value passed by casting through the pointer, nothing to outlive. */
static void *value_worker(void *p) {
    return (void *)(size_t)((size_t)p * 2u + 1u);
}

/* Correct pattern 4: the creator waits for the child to copy the argument before reusing it. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int taken;
    int shared;
    int copies[T];
} Handoff;

typedef struct {
    Handoff *h;
    int slot;
} HandoffArg;

static void *handoff_worker(void *p) {
    HandoffArg *a = p;
    Handoff *h = a->h;
    pthread_mutex_lock(&h->mu);
    h->copies[a->slot] = h->shared; /* copy the transient value */
    h->taken = 1;
    pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

int main(void) {
    Slot slots[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        slots[i].index = i;
        snprintf(slots[i].name, sizeof slots[i].name, "job-%d-%s", i, (i % 2) ? "odd" : "even");
        slots[i].result = -1;
        check(pthread_create(&th[i], NULL, slot_worker, &slots[i]) == 0, "create slot");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < T; i++) {
        check(slots[i].result == i * 100 + (int)strlen(slots[i].name), "slot result");
        printf("slot %d name=%s result=%d\n", i, slots[i].name, slots[i].result);
    }

    int outs[T];
    for (int i = 0; i < T; i++) {
        HeapArg *h = malloc(sizeof *h);
        check(h != NULL, "malloc");
        char buf[16];
        snprintf(buf, sizeof buf, "label%d", i * i);
        h->value = i + 10;
        h->label = strdup(buf);
        h->out = &outs[i];
        check(pthread_create(&th[i], NULL, heap_worker, h) == 0, "create heap");
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    for (int i = 0; i < T; i++) {
        char buf[16];
        snprintf(buf, sizeof buf, "label%d", i * i);
        check(outs[i] == (i + 10) * 3 + (int)strlen(buf), "heap result");
    }
    printf("heap results:");
    for (int i = 0; i < T; i++)
        printf(" %d", outs[i]);
    printf("\n");

    for (size_t i = 0; i < T; i++)
        pthread_create(&th[i], NULL, value_worker, (void *)(i + 20));
    printf("value results:");
    for (size_t i = 0; i < T; i++) {
        void *r;
        pthread_join(th[i], &r);
        check((size_t)r == (i + 20) * 2 + 1, "value result");
        printf(" %zu", (size_t)r);
    }
    printf("\n");

    /* one stack variable reused for every child, made safe by the handoff */
    Handoff h = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, {0}};
    HandoffArg ha = {&h, 0};
    for (int i = 0; i < T; i++) {
        pthread_mutex_lock(&h.mu);
        h.shared = 1000 + i * 7;
        h.taken = 0;
        ha.slot = i;
        pthread_mutex_unlock(&h.mu);
        check(pthread_create(&th[i], NULL, handoff_worker, &ha) == 0, "create handoff");
        pthread_mutex_lock(&h.mu);
        while (!h.taken)
            pthread_cond_wait(&h.cv, &h.mu);
        pthread_mutex_unlock(&h.mu);
    }
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    printf("handoff copies:");
    for (int i = 0; i < T; i++) {
        check(h.copies[i] == 1000 + i * 7, "handoff copy");
        printf(" %d", h.copies[i]);
    }
    printf("\n");
    return 0;
}
