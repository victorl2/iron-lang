/*
 * title: Scripted ABA scenario: untagged vs tagged CAS, tag wraparound
 * topic: concurrency
 * covers: ABA problem, compare_exchange on stale head, version tags, 16-bit tag wraparound false positive, deterministic interleaving
 * deps: libc, pthread
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The interleaving is scripted step by step on one thread, so the outcome is deterministic. A
 * "victim" starts a pop: it reads top=A and A.next=B, then is paused. While it is paused an
 * "adversary" pops A, pops B, and pushes A back, so top is A again but A.next is now C
 * (B is gone). When the victim resumes and does CAS(top, A, B):
 *   - with a bare index the CAS succeeds and installs B, a node that is no longer in the stack;
 *   - with an (index, tag) pair the tag changed, so the CAS fails and the victim retries.
 * Finally, with only a 16-bit tag, an adversary that performs exactly 65536 push/pop pairs
 * brings the tag back to its old value and the stale CAS succeeds again; a 32-bit tag does not.
 */
enum { NODES = 4, NIL = 0 }; /* node ids 1..3, links are ids, 0 = end */

static int next_of[NODES + 1];

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void show(const char *label, unsigned top) {
    printf("%-28s stack:", label);
    int guard = 0;
    for (unsigned n = top; n != NIL && guard < 8; n = (unsigned)next_of[n], guard++)
        printf(" %u", n);
    if (top == NIL)
        printf(" (empty)");
    printf("\n");
}

static void reset_stack(void) {
    next_of[1] = 2; /* A=1 -> B=2 -> C=3 */
    next_of[2] = 3;
    next_of[3] = NIL;
}

/* ---- untagged ---- */
static atomic_uint top_plain;

static unsigned pop_plain(void) {
    unsigned t = atomic_load(&top_plain);
    atomic_store(&top_plain, (unsigned)next_of[t]);
    return t;
}
static void push_plain(unsigned n) {
    next_of[n] = (int)atomic_load(&top_plain);
    atomic_store(&top_plain, n);
}

/* ---- tagged: 64-bit word = tag << 32 | id (tag width chosen by mask) ---- */
static atomic_uint_least64_t top_tag;
static uint64_t tag_mask;

static uint64_t pack(uint64_t tag, unsigned id) { return ((tag & tag_mask) << 32) | id; }
static unsigned pop_tag(void) {
    uint64_t w = atomic_load(&top_tag);
    unsigned id = (unsigned)w;
    atomic_store(&top_tag, pack((w >> 32) + 1, (unsigned)next_of[id]));
    return id;
}
static void push_tag(unsigned n) {
    uint64_t w = atomic_load(&top_tag);
    next_of[n] = (int)(unsigned)w;
    atomic_store(&top_tag, pack((w >> 32) + 1, n));
}

int main(void) {
    printf("-- untagged head --\n");
    reset_stack();
    atomic_store(&top_plain, 1);
    show("initial", atomic_load(&top_plain));
    unsigned seen_top = atomic_load(&top_plain);
    unsigned seen_next = (unsigned)next_of[seen_top];
    printf("victim reads top=%u next=%u, then pauses\n", seen_top, seen_next);
    unsigned a = pop_plain();
    unsigned b = pop_plain();
    (void)b;
    push_plain(a);
    show("after adversary pop,pop,push", atomic_load(&top_plain));
    unsigned expect = seen_top;
    int ok = atomic_compare_exchange_strong(&top_plain, &expect, seen_next);
    printf("victim CAS(top, %u, %u): %s\n", seen_top, seen_next, ok ? "succeeds" : "fails");
    check(ok, "untagged CAS is fooled by ABA");
    show("after victim CAS", atomic_load(&top_plain));
    check(atomic_load(&top_plain) == 2, "node 2 was resurrected");
    printf("top now points at node 2, which was already popped: corrupted\n");

    printf("-- tagged head (32-bit tag) --\n");
    tag_mask = 0xffffffffull;
    reset_stack();
    atomic_store(&top_tag, pack(0, 1));
    uint64_t seen = atomic_load(&top_tag);
    unsigned sn = (unsigned)next_of[(unsigned)seen];
    unsigned x = pop_tag();
    (void)pop_tag();
    push_tag(x);
    show("after adversary pop,pop,push", (unsigned)atomic_load(&top_tag));
    uint64_t e = seen;
    ok = atomic_compare_exchange_strong(&top_tag, &e, pack((seen >> 32) + 1, sn));
    printf("victim CAS with stale tag %llu: %s\n", (unsigned long long)(seen >> 32), ok ? "succeeds" : "fails");
    check(!ok, "tagged CAS detects the change");
    printf("current tag is %llu, victim must retry\n", (unsigned long long)(atomic_load(&top_tag) >> 32));

    printf("-- tag width and wraparound --\n");
    const unsigned widths[2] = {16, 32};
    for (int w = 0; w < 2; w++) {
        tag_mask = (widths[w] == 32) ? 0xffffffffull : 0xffffull;
        reset_stack();
        atomic_store(&top_tag, pack(0, 1));
        seen = atomic_load(&top_tag);
        sn = (unsigned)next_of[(unsigned)seen];
        /* adversary: pop A then push A back, 65536 times (each op bumps the tag) */
        for (long i = 0; i < 65536 / 2; i++) {
            unsigned n1 = pop_tag();
            push_tag(n1);
        }
        e = seen;
        ok = atomic_compare_exchange_strong(&top_tag, &e, pack((seen >> 32) + 1, sn));
        printf("%2u-bit tag after 65536 tag bumps: stale CAS %s\n", widths[w], ok ? "succeeds (false positive)" : "fails");
        check(ok == (widths[w] == 16), "wraparound only at 16 bits");
    }
    return 0;
}
