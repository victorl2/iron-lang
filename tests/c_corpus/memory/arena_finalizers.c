/*
 * title: Arena with finalizer chain run in reverse order on rollback
 * topic: memory
 * covers: arena destructors, finalizer records inside the arena, LIFO destruction order, rollback to mark running only newer finalizers, resource state machine, function pointer dispatch
 * deps: libc
 */
#define SEED 0xF1A411E2ULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}
static void pat_fill(void *vp, size_t n, unsigned tag) {
    unsigned char *p = vp;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u);
}
static int pat_ok(const void *vp, size_t n, unsigned tag) {
    const unsigned char *p = vp;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u)) return 0;
    return 1;
}

#define CAP 16384u
#define MAXRES 4096

typedef void (*Dtor)(void *obj, unsigned id);

typedef struct { Dtor fn; uint32_t prev; uint32_t obj_off; uint32_t id; uint32_t pad; } Fin;   /* lives in the arena */

static _Alignas(16) unsigned char mem[CAP];
static unsigned used;
static uint32_t fin_head;             /* offset of newest Fin, 0xFFFFFFFF when none (offset 0 is a valid Fin) */
#define NOFIN 0xFFFFFFFFu
static unsigned peak;

/* resource state machine tracked outside the arena */
enum { R_NONE, R_OPEN, R_CLOSED };
static unsigned char rstate[MAXRES];
static unsigned dtor_log[MAXRES], nlog;
static unsigned long dtor_calls[4];

static void *arena_alloc(size_t n, size_t align) {
    size_t s = (used + align - 1) & ~(align - 1);
    if (s + n > CAP) return NULL;
    used = (unsigned)(s + n);
    if (used > peak) peak = used;
    return mem + s;
}

static void *arena_new(size_t n, Dtor fn, unsigned id) {
    unsigned save = used;
    unsigned char *fp = NULL;
    if (fn) { fp = arena_alloc(sizeof(Fin), 8); if (!fp) return NULL; }
    void *o = arena_alloc(n, 8);
    if (!o) { used = save; return NULL; }
    if (fn) {
        Fin f;
        f.fn = fn; f.prev = fin_head; f.obj_off = (uint32_t)((unsigned char *)o - mem); f.id = id; f.pad = 0;
        memcpy(fp, &f, sizeof f);
        fin_head = (uint32_t)(fp - mem);
    }
    return o;
}

static unsigned run_finalizers_down_to(unsigned mark) {
    unsigned n = 0;
    while (fin_head != NOFIN && fin_head >= mark) {
        Fin f;
        memcpy(&f, mem + fin_head, sizeof f);
        fin_head = f.prev;
        f.fn(mem + f.obj_off, f.id);
        n++;
    }
    return n;
}

static unsigned rollback(unsigned mark) {
    unsigned n = run_finalizers_down_to(mark);
    memset(mem + mark, 0xDD, used - mark);
    used = mark;
    return n;
}

typedef struct { unsigned id; unsigned payload[3]; } Handle;

static void close_handle(void *o, unsigned id) {
    Handle *h = o;
    CHECK(h->id == id && rstate[id] == R_OPEN);
    rstate[id] = R_CLOSED;
    dtor_log[nlog++] = id;
    dtor_calls[0]++;
}
static void release_lock(void *o, unsigned id) {
    unsigned v; memcpy(&v, o, 4);
    CHECK(v == id * 3u + 1u && rstate[id] == R_OPEN);
    rstate[id] = R_CLOSED;
    dtor_log[nlog++] = id;
    dtor_calls[1]++;
}
static void verify_buf(void *o, unsigned id) {
    CHECK(pat_ok(o, 24, id) && rstate[id] == R_OPEN);
    rstate[id] = R_CLOSED;
    dtor_log[nlog++] = id;
    dtor_calls[2]++;
}

int main(void) {
    fin_head = NOFIN;
    /* small scripted scenario first: shows exact reverse order */
    unsigned id = 1;
    unsigned created[64], ncreated = 0;
    for (int i = 0; i < 6; i++) {
        Handle *h = arena_new(sizeof(Handle), close_handle, id);
        CHECK(h);
        h->id = id; rstate[id] = R_OPEN; created[ncreated++] = id; id++;
        if (i == 2) { unsigned char *plain = arena_new(20, NULL, 0); CHECK(plain); memset(plain, 0x7, 20); }
    }
    unsigned n = rollback(0);
    printf("scripted: finalizers run=%u order:", n);
    for (unsigned i = 0; i < nlog; i++) printf(" %u", dtor_log[i]);
    printf("\n");
    for (unsigned i = 0; i < ncreated; i++) CHECK(dtor_log[i] == created[ncreated - 1 - i] && rstate[created[i]] == R_CLOSED);
    nlog = 0;

    /* randomized run with nested marks */
    unsigned marks[10], mark_created[10], nm = 0;
    static unsigned live_ids[MAXRES];
    unsigned nlive = 0, rollbacks = 0, total_run = 0, refused = 0;
    unsigned long order_hash = 1469598103934665603ULL & 0xFFFFFFFFULL;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 100;
        if (op < 70) {
            unsigned kind = rnd() % 4;
            void *o = NULL;
            if (id >= MAXRES - 1) break;
            if (kind == 0) { Handle *h = arena_new(sizeof(Handle), close_handle, id); if (h) { h->id = id; o = h; } }
            else if (kind == 1) { unsigned *l = arena_new(8, release_lock, id); if (l) { *l = id * 3u + 1u; o = l; } }
            else if (kind == 2) { unsigned char *b = arena_new(24, verify_buf, id); if (b) { pat_fill(b, 24, id); o = b; } }
            else { unsigned char *b = arena_new(1 + rnd() % 50, NULL, 0); if (!b) refused++; continue; }
            if (!o) { refused++; continue; }
            rstate[id] = R_OPEN; live_ids[nlive++] = id; id++;
        } else if (op < 85) {
            if (nm < 10) { marks[nm] = used; mark_created[nm] = nlive; nm++; }
        } else if (nm > 0) {
            nm--;
            unsigned before = nlog;
            unsigned ran = rollback(marks[nm]);
            /* exactly the resources created after the mark were finalized, newest first */
            CHECK(ran == nlive - mark_created[nm]);
            for (unsigned k = 0; k < ran; k++) CHECK(dtor_log[before + k] == live_ids[nlive - 1 - k]);
            for (unsigned k = 0; k < ran; k++) order_hash = (order_hash * 31u + dtor_log[before + k]) & 0xFFFFFFFFULL;
            nlive = mark_created[nm];
            total_run += ran; rollbacks++;
        }
    }
    total_run += rollback(0);
    for (unsigned i = 1; i < id; i++) CHECK(rstate[i] == R_CLOSED);
    CHECK(fin_head == NOFIN && used == 0);
    printf("resources created=%u finalized=%u rollbacks=%u refused=%u\n", id - 1 - 6, total_run, rollbacks, refused);
    printf("dtor calls: handle=%lu lock=%lu buf=%lu\n", dtor_calls[0] - 6, dtor_calls[1], dtor_calls[2]);
    printf("arena peak=%u of %u, order hash=%lu\n", peak, CAP, order_hash);
    return 0;
}
