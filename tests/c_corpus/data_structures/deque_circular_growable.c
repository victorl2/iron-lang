/*
 * title: Growable circular deque with random access
 * topic: data_structures
 * covers: deque, circular buffer growth, unwrap on resize, indexed access, rotation, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x7A3B9C1D5E2F4681ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 15); }

typedef struct { int *buf; size_t cap, head, len; int grows; } Deque;

static void dq_init(Deque *d) { d->buf = NULL; d->cap = d->head = d->len = 0; d->grows = 0; }
static size_t idx(const Deque *d, size_t i) { return (d->head + i) & (d->cap - 1); }
static void dq_grow(Deque *d) {
    size_t nc = d->cap ? d->cap * 2 : 4;
    int *nb = malloc(nc * sizeof *nb); CHECK(nb);
    for (size_t i = 0; i < d->len; i++) nb[i] = d->buf[idx(d, i)];
    free(d->buf);
    d->buf = nb; d->cap = nc; d->head = 0; d->grows++;
}
static void push_back(Deque *d, int v) { if (d->len == d->cap) dq_grow(d); d->buf[idx(d, d->len)] = v; d->len++; }
static void push_front(Deque *d, int v) {
    if (d->len == d->cap) dq_grow(d);
    d->head = (d->head + d->cap - 1) & (d->cap - 1);
    d->buf[d->head] = v; d->len++;
}
static int pop_front(Deque *d) { CHECK(d->len); int v = d->buf[d->head]; d->head = (d->head + 1) & (d->cap - 1); d->len--; return v; }
static int pop_back(Deque *d) { CHECK(d->len); d->len--; return d->buf[idx(d, d->len)]; }
static int *at(Deque *d, size_t i) { CHECK(i < d->len); return &d->buf[idx(d, i)]; }
/* rotate so element k becomes the front: k pops from the front pushed to the back */
static void rotate_left(Deque *d, size_t k) { for (size_t i = 0; i < k; i++) push_back(d, pop_front(d)); }
static void rotate_right(Deque *d, size_t k) { for (size_t i = 0; i < k; i++) push_front(d, pop_back(d)); }
/* is the used region currently wrapped around the end of the buffer? */
static int wrapped(const Deque *d) { return d->cap && d->head + d->len > d->cap; }

#define MAXN 3000
int main(void) {
    Deque d; dq_init(&d);
    static int m[MAXN]; size_t mn = 0;
    long cnt[7] = {0}, wrapped_steps = 0;
    for (int step = 0; step < 30000; step++) {
        unsigned op = rnd() % 100;
        int v = (int)(rnd() % 100000);
        if (mn > 2500) op = 70;
        if (op < 22) { push_back(&d, v); m[mn++] = v; cnt[0]++; }
        else if (op < 44) { push_front(&d, v); memmove(m + 1, m, mn * sizeof(int)); m[0] = v; mn++; cnt[1]++; }
        else if (op < 60 && mn) { CHECK(pop_front(&d) == m[0]); memmove(m, m + 1, --mn * sizeof(int)); cnt[2]++; }
        else if (op < 76 && mn) { CHECK(pop_back(&d) == m[--mn]); cnt[3]++; }
        else if (op < 88 && mn) { size_t i = rnd() % mn; CHECK(*at(&d, i) == m[i]); *at(&d, i) = v; m[i] = v; cnt[4]++; }
        else if (op < 94 && mn) {
            size_t k = rnd() % (mn + 1) % 9;
            rotate_left(&d, k);
            for (size_t r = 0; r < k; r++) { int f = m[0]; memmove(m, m + 1, (mn - 1) * sizeof(int)); m[mn - 1] = f; }
            cnt[5]++;
        } else if (mn) {
            size_t k = rnd() % (mn + 1) % 9;
            rotate_right(&d, k);
            for (size_t r = 0; r < k; r++) { int l = m[mn - 1]; memmove(m + 1, m, (mn - 1) * sizeof(int)); m[0] = l; }
            cnt[6]++;
        }
        CHECK(d.len == mn);
        if (wrapped(&d)) wrapped_steps++;
        if (step % 7 == 0) for (size_t i = 0; i < mn; i++) CHECK(*at(&d, i) == m[i]);
    }
    printf("push_back=%ld push_front=%ld pop_front=%ld pop_back=%ld set=%ld rot_left=%ld rot_right=%ld\n",
           cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6]);
    printf("len=%zu cap=%zu grows=%d wrapped_steps=%ld\n", d.len, d.cap, d.grows, wrapped_steps);
    printf("first:"); for (size_t i = 0; i < 5 && i < d.len; i++) printf(" %d", *at(&d, i));
    printf("\nlast:"); for (size_t i = 0; i < 5 && i < d.len; i++) printf(" %d", *at(&d, d.len - 1 - i));
    printf("\n");
    free(d.buf);
    return 0;
}
