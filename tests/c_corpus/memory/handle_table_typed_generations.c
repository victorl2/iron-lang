/*
 * title: Handle table with type tags and generations
 * topic: memory
 * covers: opaque handles, bit-packed generation/type/index, stale handle detection, free-list reuse
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* handle layout: [gen:12][type:4][index:16] */
typedef uint32_t Handle;
enum { T_TEXTURE = 1, T_SOUND = 2, T_MESH = 3 };
enum { OK, ERR_NULL, ERR_STALE, ERR_TYPE, ERR_FULL };

#define H_INDEX(h) ((h) & 0xFFFFu)
#define H_TYPE(h) (((h) >> 16) & 0xFu)
#define H_GEN(h) ((h) >> 20)

typedef struct {
    uint16_t gen;
    uint8_t type;
    uint8_t used;
    int next_free;
    int payload;
} Slot;

#define CAP 6
static Slot slots[CAP];
static int free_head;
static int destroyed_payload_sum;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void table_init(void) {
    for (int i = 0; i < CAP; i++) {
        slots[i].gen = 1;
        slots[i].used = 0;
        slots[i].next_free = i + 1 < CAP ? i + 1 : -1;
    }
    free_head = 0;
}

static int h_create(int type, int payload, Handle *out) {
    if (free_head < 0)
        return ERR_FULL;
    int i = free_head;
    free_head = slots[i].next_free;
    slots[i].used = 1;
    slots[i].type = (uint8_t)type;
    slots[i].payload = payload;
    *out = ((Handle)slots[i].gen << 20) | ((Handle)type << 16) | (Handle)i;
    return OK;
}

static int h_resolve(Handle h, int want_type, Slot **out) {
    if (h == 0)
        return ERR_NULL;
    unsigned i = H_INDEX(h);
    if (i >= CAP || !slots[i].used || slots[i].gen != H_GEN(h))
        return ERR_STALE;
    if (want_type && slots[i].type != H_TYPE(h))
        return ERR_TYPE;
    if (want_type && slots[i].type != want_type)
        return ERR_TYPE;
    *out = &slots[i];
    return OK;
}

static int h_destroy(Handle h) {
    Slot *s;
    int r = h_resolve(h, 0, &s);
    if (r != OK)
        return r;
    destroyed_payload_sum += s->payload;
    s->used = 0;
    s->gen = (uint16_t)((s->gen + 1) & 0xFFF);
    if (s->gen == 0)
        s->gen = 1; /* never issue generation 0 so handle 0 stays invalid */
    s->next_free = free_head;
    free_head = (int)(s - slots);
    return OK;
}

static const char *name(int r) {
    static const char *n[] = {"ok", "null", "stale", "wrong-type", "full"};
    return n[r];
}

int main(void) {
    table_init();
    Handle h[CAP + 1];
    int types[] = {T_TEXTURE, T_SOUND, T_MESH, T_TEXTURE, T_SOUND, T_MESH};
    for (int i = 0; i < CAP; i++)
        check(h_create(types[i], 100 + i, &h[i]) == OK, "create");
    Handle extra;
    printf("create when full: %s\n", name(h_create(T_MESH, 0, &extra)));

    Slot *s;
    printf("resolve h2 as mesh: %s\n", name(h_resolve(h[2], T_MESH, &s)));
    printf("resolve h2 as sound: %s\n", name(h_resolve(h[2], T_SOUND, &s)));
    printf("resolve null: %s\n", name(h_resolve(0, 0, &s)));

    const char *first_destroy = name(h_destroy(h[1]));
    const char *second_destroy = name(h_destroy(h[1]));
    printf("destroy h1: %s, again: %s\n", first_destroy, second_destroy);
    printf("stale resolve: %s\n", name(h_resolve(h[1], 0, &s)));
    Handle re;
    check(h_create(T_MESH, 555, &re) == OK, "recreate");
    printf("reused index: %s, gen %u -> %u, type %u\n", H_INDEX(re) == H_INDEX(h[1]) ? "yes" : "no",
           (unsigned)H_GEN(h[1]), (unsigned)H_GEN(re), (unsigned)H_TYPE(re));
    printf("old handle vs new slot: %s\n", name(h_resolve(h[1], 0, &s)));
    check(h_resolve(re, T_MESH, &s) == OK && s->payload == 555, "new resolves");

    /* churn one slot until the 12-bit generation wraps */
    Handle cur = re;
    Handle first_old = cur;
    int wrapped_at = -1;
    for (int i = 1; i <= 5000; i++) {
        check(h_destroy(cur) == OK, "churn destroy");
        check(h_create(T_SOUND, i, &cur) == OK, "churn create");
        if (wrapped_at < 0 && H_GEN(cur) < H_GEN(first_old))
            wrapped_at = i;
    }
    printf("generation wrapped after %d churns, final gen %u\n", wrapped_at, (unsigned)H_GEN(cur));
    printf("ancient handle still stale? %s\n",
           h_resolve(first_old, 0, &s) == OK ? "no (aliased after wrap)" : "yes");
    printf("destroyed payload sum: %d\n", destroyed_payload_sum);
    return 0;
}
