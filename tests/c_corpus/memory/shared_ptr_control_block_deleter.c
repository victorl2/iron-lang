/*
 * title: Non-intrusive shared pointer with control block and deleter
 * topic: memory
 * covers: separate control block, custom deleters, aliasing shared pointers, use counts
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int use;
    void *obj;
    void (*deleter)(void *);
} Ctl;

typedef struct {
    Ctl *ctl;
    void *ptr; /* may differ from ctl->obj for aliasing constructors */
} Shared;

static char events[512];
static int live_ctl;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void note(const char *s) {
    size_t n = strlen(events);
    check(n + strlen(s) + 2 < sizeof events, "event log");
    memcpy(events + n, s, strlen(s) + 1);
}

static Shared sp_make(void *obj, void (*del)(void *)) {
    Shared s;
    s.ctl = malloc(sizeof *s.ctl);
    check(s.ctl != NULL, "alloc");
    s.ctl->use = 1;
    s.ctl->obj = obj;
    s.ctl->deleter = del;
    s.ptr = obj;
    live_ctl++;
    return s;
}

static Shared sp_copy(Shared s) {
    if (s.ctl)
        s.ctl->use++;
    return s;
}

static Shared sp_alias(Shared owner, void *sub) {
    Shared s = sp_copy(owner);
    s.ptr = sub;
    return s;
}

static void sp_reset(Shared *s) {
    if (s->ctl && --s->ctl->use == 0) {
        s->ctl->deleter(s->ctl->obj);
        free(s->ctl);
        live_ctl--;
    }
    s->ctl = NULL;
    s->ptr = NULL;
}

static int sp_count(Shared s) { return s.ctl ? s.ctl->use : 0; }

typedef struct {
    int header;
    int fields[4];
} Record;

static void del_record(void *p) {
    note("R");
    free(p);
}

static void del_array(void *p) {
    note("A");
    free(p);
}

static void del_custom(void *p) {
    /* a resource whose "release" is not plain free */
    int *flag = p;
    *flag = 0;
    note("C");
}

int main(void) {
    Record *r = malloc(sizeof *r);
    check(r != NULL, "alloc");
    r->header = 7;
    for (int i = 0; i < 4; i++)
        r->fields[i] = i * i;
    Shared whole = sp_make(r, del_record);
    Shared f2 = sp_alias(whole, &r->fields[2]);
    Shared f3 = sp_alias(whole, &r->fields[3]);
    printf("use count after aliasing: %d\n", sp_count(whole));
    printf("alias values: %d %d\n", *(int *)f2.ptr, *(int *)f3.ptr);

    sp_reset(&whole);
    printf("after resetting owner: use=%d, alias still reads %d\n", sp_count(f2), *(int *)f2.ptr);
    sp_reset(&f2);
    check(live_ctl == 1, "one ctl live");
    sp_reset(&f3);
    check(live_ctl == 0, "ctl freed");
    printf("events: %s\n", events);

    int *arr = malloc(10 * sizeof *arr);
    check(arr != NULL, "alloc");
    for (int i = 0; i < 10; i++)
        arr[i] = i;
    Shared a1 = sp_make(arr, del_array);
    Shared copies[5];
    for (int i = 0; i < 5; i++)
        copies[i] = sp_copy(a1);
    printf("array use=%d\n", sp_count(a1));
    for (int i = 0; i < 5; i++)
        sp_reset(&copies[i]);
    printf("array use=%d\n", sp_count(a1));
    sp_reset(&a1);

    int flag = 1;
    Shared c = sp_make(&flag, del_custom);
    Shared c2 = sp_copy(c);
    sp_reset(&c);
    printf("flag while shared: %d\n", flag);
    sp_reset(&c2);
    printf("flag after last: %d\n", flag);
    printf("events: %s\n", events);
    check(strcmp(events, "RAC") == 0, "event order");
    check(live_ctl == 0, "no ctl leak");
    return 0;
}
