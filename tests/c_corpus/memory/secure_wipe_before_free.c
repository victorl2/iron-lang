/*
 * title: Zeroing secrets before free with a graveyard allocator
 * topic: memory
 * covers: secure zeroing, volatile stores, realloc shrink wipe, allocator that records freed contents
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An allocator that never returns memory to malloc immediately: it snapshots the bytes
 * that the block held at the moment of free, so we can see what a later allocation would inherit. */
#define MAXG 16
static unsigned char graveyard[MAXG][64];
static size_t grave_len[MAXG];
static int ngrave;

static void g_free(void *p, size_t n) {
    if (ngrave < MAXG) {
        memcpy(graveyard[ngrave], p, n);
        grave_len[ngrave++] = n;
    }
    free(p);
}

static void secure_zero(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void secure_free(void *p, size_t n) {
    secure_zero(p, n);
    g_free(p, n);
}

/* shrinking must wipe the tail it gives up */
static unsigned char *secure_shrink(unsigned char *p, size_t old_n, size_t new_n) {
    secure_zero(p + new_n, old_n - new_n);
    return p; /* keep the block; a real allocator would split it */
}

static int contains(const unsigned char *hay, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return 1;
    return 0;
}

static size_t nonzero(const unsigned char *p, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) c += p[i] != 0;
    return c;
}

typedef struct { char user[16]; unsigned char key[24]; unsigned counter; } Session;

static void fill_session(Session *s, unsigned seed) {
    memset(s, 0, sizeof *s);
    snprintf(s->user, sizeof s->user, "user%u", seed);
    unsigned x = seed * 2654435761u + 1;
    for (size_t i = 0; i < sizeof s->key; i++) { x = x * 1103515245u + 12345u; s->key[i] = (unsigned char)(x >> 16) | 1; }
    s->counter = seed * 3;
}

int main(void) {
    const char *secret = "hunter2-PASSWORD";
    size_t n = 32;

    /* 1. naive free leaves the secret in the recycled bytes */
    unsigned char *a = malloc(n);
    memset(a, 0, n);
    memcpy(a + 4, secret, strlen(secret));
    g_free(a, n);
    /* 2. secure free */
    unsigned char *b = malloc(n);
    memset(b, 0, n);
    memcpy(b + 4, secret, strlen(secret));
    secure_free(b, n);
    printf("naive free : secret visible=%d nonzero=%zu\n", contains(graveyard[0], n, secret), nonzero(graveyard[0], n));
    printf("secure free: secret visible=%d nonzero=%zu\n", contains(graveyard[1], n, secret), nonzero(graveyard[1], n));
    if (!contains(graveyard[0], n, secret) || contains(graveyard[1], n, secret)) return 1;

    /* 3. shrink wipes the surrendered tail */
    unsigned char *c = malloc(48);
    for (int i = 0; i < 48; i++) c[i] = (unsigned char)(0x80 + i);
    c = secure_shrink(c, 48, 16);
    size_t head = nonzero(c, 16), tail = nonzero(c + 16, 32);
    printf("shrink 48->16: head nonzero=%zu tail nonzero=%zu\n", head, tail);
    if (head != 16 || tail != 0) return 1;
    secure_free(c, 16);

    /* 4. struct with key material: wipe entire object, not just the key */
    Session *s = malloc(sizeof *s);
    fill_session(s, 7);
    size_t sn = sizeof *s;
    printf("session before wipe: user=%s counter=%u key nonzero=%zu\n", s->user, s->counter, nonzero(s->key, sizeof s->key));
    unsigned char snap[64];
    memcpy(snap, s, sn);
    secure_free(s, sn);
    int idx = ngrave - 1;
    printf("session graveyard nonzero after secure_free = %zu (was %zu)\n", nonzero(graveyard[idx], sn), nonzero(snap, sn));
    if (nonzero(graveyard[idx], sn) != 0) return 1;

    /* 5. batch of sessions */
    size_t total_before = 0, total_after = 0;
    int first = ngrave;
    for (unsigned i = 1; i <= 5; i++) {
        Session *t = malloc(sizeof *t);
        fill_session(t, i * 11);
        memcpy(snap, t, sizeof *t);
        total_before += nonzero(snap, sizeof *t);
        secure_free(t, sizeof *t);
    }
    for (int i = first; i < ngrave; i++) total_after += nonzero(graveyard[i], sizeof(Session));
    printf("5 sessions: nonzero bytes before=%zu after=%zu\n", total_before, total_after);
    return total_after == 0 ? 0 : 1;
}
