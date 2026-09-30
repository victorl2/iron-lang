/*
 * title: Mutex-protected LIFO stack with node conservation
 * topic: concurrency
 * covers: shared stack, push/pop under lock, node recycling, no lost or duplicated nodes
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { T = 6, NODES = 60, OPS = 3000 };

typedef struct Node {
    int id;
    int hops; /* how many times this node has been moved */
    struct Node *next;
} Node;

typedef struct {
    pthread_mutex_t mu;
    Node *top;
    int size;
} Stack;

static Stack stacks[2];
static Node pool[NODES];

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void push(Stack *s, Node *n) {
    pthread_mutex_lock(&s->mu);
    n->next = s->top;
    s->top = n;
    s->size++;
    pthread_mutex_unlock(&s->mu);
}

static Node *pop(Stack *s) {
    pthread_mutex_lock(&s->mu);
    Node *n = s->top;
    if (n) {
        s->top = n->next;
        s->size--;
        n->next = NULL;
    }
    pthread_mutex_unlock(&s->mu);
    return n;
}

typedef struct {
    unsigned seed;
    long moves;
    long empty_pops;
} Arg;

static void *worker(void *p) {
    Arg *a = p;
    for (int i = 0; i < OPS; i++) {
        a->seed = a->seed * 1664525u + 1013904223u;
        int from = (int)((a->seed >> 20) & 1u);
        Node *n = pop(&stacks[from]);
        if (!n) {
            a->empty_pops++;
            continue;
        }
        n->hops++; /* exclusively owned while off both stacks */
        push(&stacks[1 - from], n);
        a->moves++;
    }
    return NULL;
}

int main(void) {
    for (int i = 0; i < 2; i++) {
        pthread_mutex_init(&stacks[i].mu, NULL);
        stacks[i].top = NULL;
        stacks[i].size = 0;
    }
    for (int i = 0; i < NODES; i++) {
        pool[i].id = i;
        pool[i].hops = 0;
        push(&stacks[0], &pool[i]);
    }
    Arg args[T];
    pthread_t th[T];
    for (int i = 0; i < T; i++) {
        args[i].seed = 0x5eed0000u + (unsigned)i * 65537u;
        args[i].moves = 0;
        args[i].empty_pops = 0;
        check(pthread_create(&th[i], NULL, worker, &args[i]) == 0, "create");
    }
    long moves = 0;
    for (int i = 0; i < T; i++) {
        pthread_join(th[i], NULL);
        moves += args[i].moves;
    }
    check(stacks[0].size + stacks[1].size == NODES, "node count conserved");
    int seen[NODES] = {0};
    long hops = 0;
    for (int s = 0; s < 2; s++) {
        int len = 0;
        for (Node *n = stacks[s].top; n; n = n->next) {
            check(n->id >= 0 && n->id < NODES && !seen[n->id], "node appears exactly once");
            seen[n->id] = 1;
            hops += n->hops;
            len++;
        }
        check(len == stacks[s].size, "size field matches walk");
    }
    for (int i = 0; i < NODES; i++)
        check(seen[i], "no node lost");
    check(hops == moves, "hops equal moves");
    printf("nodes=%d\n", stacks[0].size + stacks[1].size);
    printf("every node present once: yes\n");
    printf("hop count equals move count: %s\n", hops == moves ? "yes" : "no");
    return 0;
}
