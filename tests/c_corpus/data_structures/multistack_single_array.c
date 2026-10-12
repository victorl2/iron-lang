/*
 * title: Multiple stacks sharing one array with rebalancing
 * topic: data_structures
 * covers: multi-stack, shared array, Knuth stack repacking, overflow shifting, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x1F83D9ABFB41BD6BULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 25); }

#define K 5
#define TOTAL 60

/* Stack i occupies data[base[i] .. base[i] + size[i]) with room up to base[i + 1]. */
typedef struct { int data[TOTAL]; int base[K + 1]; int size[K]; long repacks, moved; } Multi;

static void mu_init(Multi *m) {
    memset(m, 0, sizeof *m);
    for (int i = 0; i <= K; i++) m->base[i] = i * TOTAL / K;
}
static int mu_total(const Multi *m) { int t = 0; for (int i = 0; i < K; i++) t += m->size[i]; return t; }

/* Redistribute free space proportionally to growth pressure: stacks that are currently
 * larger get more room. Stack `hot` receives one extra slot of room. */
static void mu_repack(Multi *m, int hot) {
    int used = mu_total(m), free_slots = TOTAL - used;
    CHECK(free_slots > 0);
    int room[K], remaining = free_slots;
    /* the hot stack gets half of the free space, the rest is split by usage */
    room[hot] = (free_slots + 1) / 2; remaining -= room[hot];
    int others = 0; for (int i = 0; i < K; i++) if (i != hot) others += m->size[i] + 1;
    int given = 0;
    for (int i = 0; i < K; i++) if (i != hot) { room[i] = remaining * (m->size[i] + 1) / others; given += room[i]; }
    room[hot] += remaining - given;
    int nb[K + 1]; nb[0] = 0;
    for (int i = 0; i < K; i++) nb[i + 1] = nb[i] + m->size[i] + room[i];
    CHECK(nb[K] == TOTAL);
    /* move stacks into new positions; low-moving-left first, then right-moving from the top */
    for (int i = 0; i < K; i++) if (nb[i] <= m->base[i]) { memmove(m->data + nb[i], m->data + m->base[i], (size_t)m->size[i] * sizeof(int)); m->moved += m->size[i]; }
    for (int i = K - 1; i >= 0; i--) if (nb[i] > m->base[i]) { memmove(m->data + nb[i], m->data + m->base[i], (size_t)m->size[i] * sizeof(int)); m->moved += m->size[i]; }
    memcpy(m->base, nb, sizeof nb);
    m->repacks++;
}
static int mu_push(Multi *m, int s, int v) {
    if (m->base[s] + m->size[s] == m->base[s + 1]) {
        if (mu_total(m) == TOTAL) return 0;
        mu_repack(m, s);
        CHECK(m->base[s] + m->size[s] < m->base[s + 1]);
    }
    m->data[m->base[s] + m->size[s]++] = v;
    return 1;
}
static int mu_pop(Multi *m, int s, int *v) { if (!m->size[s]) return 0; *v = m->data[m->base[s] + --m->size[s]]; return 1; }

int main(void) {
    Multi m; mu_init(&m);
    int model[K][TOTAL], mn[K] = {0};
    long pushes = 0, pops = 0, full = 0, empty = 0;
    int skew = 0;
    for (int step = 0; step < 30000; step++) {
        /* skewed traffic: one stack is hot for a while, and it changes */
        if (step % 1500 == 0) skew = (int)(rnd() % K);
        int s = (rnd() % 3) ? skew : (int)(rnd() % K);
        int total = 0; for (int i = 0; i < K; i++) total += mn[i];
        int ppush = step % 3000 < 1500 ? 75 : 30;
        if ((int)(rnd() % 100) < ppush) {
            int v = (int)(rnd() % 1000);
            int ok = mu_push(&m, s, v);
            CHECK(ok == (total < TOTAL));
            if (ok) { model[s][mn[s]++] = v; pushes++; } else full++;
        } else {
            int v = -1; int ok = mu_pop(&m, s, &v);
            CHECK(ok == (mn[s] > 0));
            if (ok) { CHECK(v == model[s][--mn[s]]); pops++; } else empty++;
        }
        for (int i = 0; i < K; i++) {
            CHECK(m.size[i] == mn[i]);
            CHECK(m.base[i] + m.size[i] <= m.base[i + 1]);
            CHECK(memcmp(m.data + m.base[i], model[i], (size_t)mn[i] * sizeof(int)) == 0);
        }
        CHECK(m.base[0] == 0 && m.base[K] == TOTAL);
    }
    printf("push=%ld pop=%ld full=%ld empty=%ld\n", pushes, pops, full, empty);
    printf("repacks=%ld moved=%ld\n", m.repacks, m.moved);
    printf("sizes:"); for (int i = 0; i < K; i++) printf(" %d", m.size[i]);
    printf("\nbases:"); for (int i = 0; i <= K; i++) printf(" %d", m.base[i]);
    printf("\n");
    return 0;
}
