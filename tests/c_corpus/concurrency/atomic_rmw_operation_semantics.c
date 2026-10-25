/*
 * title: Semantics of every atomic read-modify-write operation
 * topic: concurrency
 * covers: fetch_add/sub/or/and/xor, exchange, compare_exchange expected update, signed and unsigned wrap, concurrent xor parity and exchange conservation
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Every fetch_op returns the value BEFORE the update. Part 1 walks each operation on a 32-bit
 * unsigned and a 64-bit signed atomic single-threaded and prints old/new pairs (unsigned
 * arithmetic wraps; signed values stay in range). A failed compare_exchange writes the current
 * value into `expected`. Part 2 uses threads: xor toggling leaves a parity-determined value,
 * or/and on disjoint bit ranges compose, and exchange conserves the multiset of tokens.
 */
enum { T = 6, REPS = 5001, TOK = 2000 };

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static atomic_uint xor_cell;
static atomic_ulong or_cell, and_cell;
static atomic_ulong swap_cell;
static unsigned long returned_sum[T];
static long returned_count[T];

static void *xorer(void *p) {
    unsigned id = (unsigned)(size_t)p;
    for (int i = 0; i < REPS; i++)
        atomic_fetch_xor(&xor_cell, 1u << id);
    return NULL;
}

static void *setter(void *p) {
    unsigned id = (unsigned)(size_t)p;
    for (int i = 0; i < 1000; i++) {
        atomic_fetch_or(&or_cell, 1ul << (id * 8 + (unsigned)i % 8));
        atomic_fetch_and(&and_cell, ~(1ul << (id * 8 + (unsigned)i % 8)));
    }
    return NULL;
}

static void *swapper(void *p) {
    unsigned long id = (unsigned long)(size_t)p;
    unsigned long sum = 0;
    long n = 0;
    for (unsigned long i = 1; i <= TOK; i++) {
        unsigned long token = id * 1000000ul + i;
        unsigned long got = atomic_exchange(&swap_cell, token);
        sum += got;
        n++;
    }
    returned_sum[id] = sum;
    returned_count[id] = n;
    return NULL;
}

int main(void) {
    atomic_uint u = 0xfffffff0u;
    unsigned old;
    old = atomic_fetch_add(&u, 0x20u);
    printf("fetch_add 0x20 on 0xfffffff0: old=0x%08x new=0x%08x (wraps)\n", old, atomic_load(&u));
    check(old == 0xfffffff0u && atomic_load(&u) == 0x10u, "wrap");
    old = atomic_fetch_sub(&u, 0x20u);
    printf("fetch_sub 0x20: old=0x%08x new=0x%08x (wraps back)\n", old, atomic_load(&u));
    check(atomic_load(&u) == 0xfffffff0u, "wrap back");
    old = atomic_fetch_or(&u, 0x0fu);
    printf("fetch_or 0x0f: old=0x%08x new=0x%08x\n", old, atomic_load(&u));
    old = atomic_fetch_and(&u, 0xff00ffffu);
    printf("fetch_and 0xff00ffff: old=0x%08x new=0x%08x\n", old, atomic_load(&u));
    old = atomic_fetch_xor(&u, 0xffffffffu);
    printf("fetch_xor ~0: old=0x%08x new=0x%08x\n", old, atomic_load(&u));
    old = atomic_exchange(&u, 7u);
    printf("exchange 7: old=0x%08x new=0x%08x\n", old, atomic_load(&u));
    check(old == 0x00ff0000u, "xor result"); /* ~(0xfffffff0|0x0f & 0xff00ffff) */

    unsigned expected = 5u;
    int ok = atomic_compare_exchange_strong(&u, &expected, 9u);
    printf("cas(expect 5, want 9) on 7: %s, expected now %u, value %u\n", ok ? "ok" : "fail", expected, atomic_load(&u));
    check(!ok && expected == 7u && atomic_load(&u) == 7u, "failed cas reports the current value");
    ok = atomic_compare_exchange_strong(&u, &expected, 9u);
    printf("cas(expect 7, want 9) on 7: %s, value %u\n", ok ? "ok" : "fail", atomic_load(&u));
    check(ok && atomic_load(&u) == 9u, "successful cas");

    atomic_llong s = INT64_MAX - 2;
    long long so = atomic_fetch_add(&s, 2);
    printf("signed fetch_add 2 near INT64_MAX: old=%lld new=%lld\n", so, atomic_load(&s));
    so = atomic_fetch_sub(&s, 10);
    printf("signed fetch_sub 10: old=%lld new=%lld\n", so, atomic_load(&s));
    atomic_llong neg = -5;
    so = atomic_fetch_add(&neg, -7);
    printf("signed fetch_add -7 on -5: old=%lld new=%lld\n", so, atomic_load(&neg));
    check(atomic_load(&neg) == -12, "negative add");

    /* Part 2: concurrent */
    pthread_t th[T];
    atomic_store(&xor_cell, 0u);
    for (unsigned i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, xorer, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    /* each thread toggled its own bit REPS (odd) times: every bit ends set */
    check(atomic_load(&xor_cell) == (1u << T) - 1u, "xor parity");
    printf("xor toggles: each of %d bits toggled %d times (odd) -> 0x%02x\n", T, REPS, atomic_load(&xor_cell));

    atomic_store(&or_cell, 0ul);
    atomic_store(&and_cell, ~0ul);
    for (unsigned i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, setter, (void *)(size_t)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    unsigned long low48 = (1ul << (T * 8)) - 1ul;
    check(atomic_load(&or_cell) == low48, "or filled 48 bits");
    check((atomic_load(&and_cell) & low48) == 0, "and cleared low 48 bits");
    printf("or set bits 0..%d: 0x%012lx, and cleared them: low bits 0x%012lx\n", T * 8 - 1,
           (unsigned long)atomic_load(&or_cell), (unsigned long)(atomic_load(&and_cell) & low48));

    atomic_store(&swap_cell, 0ul);
    for (unsigned long i = 0; i < T; i++)
        check(pthread_create(&th[i], NULL, swapper, (void *)i) == 0, "create");
    for (int i = 0; i < T; i++)
        pthread_join(th[i], NULL);
    unsigned long got = atomic_load(&swap_cell), put = 0;
    long n = 0;
    for (unsigned long id = 0; id < T; id++) {
        got += returned_sum[id];
        n += returned_count[id];
        for (unsigned long i = 1; i <= TOK; i++)
            put += id * 1000000ul + i;
    }
    check(got == put, "exchange conserves tokens (returned + final == put + initial 0)");
    printf("exchange chain: %ld swaps, token sum conserved: %lu\n", n, put);
    return 0;
}
