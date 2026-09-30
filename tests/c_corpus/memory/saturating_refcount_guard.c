/*
 * title: Saturating reference counts with underflow and over-release detection
 * topic: memory
 * covers: refcount saturation, sticky pinned objects, underflow trap, retain/release audit, deliberate leak on saturation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 8-bit count so saturation can be reached in a test. 255 = saturated: never freed, never decremented. */
#define SAT 255

typedef struct Obj {
    uint8_t rc;
    int id;
    struct Obj *next;
} Obj;

static Obj *all;
static int live_objs, underflows, saturated, freed_count;

static Obj *obj_new(int id) {
    Obj *o = malloc(sizeof *o);
    if (!o) exit(1);
    o->rc = 1; o->id = id; o->next = all; all = o;
    live_objs++;
    return o;
}

static void unlink_free(Obj *o) {
    for (Obj **pp = &all; *pp; pp = &(*pp)->next)
        if (*pp == o) { *pp = o->next; break; }
    free(o);
    live_objs--;
    freed_count++;
}

static Obj *retain(Obj *o) {
    if (o->rc == SAT) return o;
    if (o->rc == SAT - 1) { saturated++; }
    o->rc++;
    return o;
}

/* returns 1 if the object died */
static int release(Obj *o) {
    if (o->rc == SAT) return 0;       /* saturated: immortal */
    if (o->rc == 0) { underflows++; return 0; }
    if (--o->rc == 0) { unlink_free(o); return 1; }
    return 0;
}

int main(void) {
    /* 1. normal life cycle */
    Obj *a = obj_new(1);
    retain(a); retain(a);
    printf("rc after 2 retains: %u\n", a->rc);
    int d1 = release(a), d2 = release(a), d3 = release(a);
    printf("releases died: %d %d %d, live=%d\n", d1, d2, d3, live_objs);

    /* 2. saturation: 300 retains cap at 255, then releases never free it */
    Obj *b = obj_new(2);
    for (int i = 0; i < 300; i++) retain(b);
    printf("rc after 300 retains: %u saturated events=%d\n", b->rc, saturated);
    for (int i = 0; i < 400; i++) release(b);
    printf("rc after 400 releases: %u live=%d (leaked by design)\n", b->rc, live_objs);

    /* 3. underflow detection using a pinned dead-object registry: release on a zero count is counted, not executed.
     * To keep memory valid we trigger it on an object we hold with rc forced to 0 but not yet freed. */
    Obj *c = obj_new(3);
    c->rc = 0;
    int died = release(c);
    printf("release at zero: died=%d underflows=%d\n", died, underflows);
    c->rc = 1;

    /* 4. random retain/release fuzz with a shadow count; the object must never be freed early */
    Obj *pool[4];
    int shadow[4];
    for (int i = 0; i < 4; i++) { pool[i] = obj_new(10 + i); shadow[i] = 1; }
    unsigned r = 5;
    int early = 0;
    for (int i = 0; i < 2000; i++) {
        r = r * 1103515245u + 12345u;
        int k = (int)((r >> 16) % 4);
        if (!pool[k]) continue;
        if ((r >> 8) % 3 != 0 && shadow[k] < 200) { retain(pool[k]); shadow[k]++; }
        else if (shadow[k] > 1) { release(pool[k]); shadow[k]--; }
        if (pool[k]->rc != shadow[k]) early++;
    }
    int sum = 0;
    for (int i = 0; i < 4; i++) sum += shadow[i];
    printf("fuzz: shadow total=%d mismatches=%d\n", sum, early);
    for (int i = 0; i < 4; i++) {
        while (shadow[i] > 0) { release(pool[i]); shadow[i]--; }
    }
    printf("after cleanup: live=%d freed=%d\n", live_objs, freed_count);

    /* release the intentionally leaked objects for a clean exit */
    while (all) { Obj *o = all; unlink_free(o); }
    (void)a;
    return early == 0 ? 0 : 1;
}
