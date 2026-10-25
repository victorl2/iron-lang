/*
 * title: Tagged pointers marking owned vs borrowed
 * topic: memory
 * covers: low-bit pointer tagging, owned/borrowed flag, clone-on-write, conditional free, uintptr_t casts
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Items are always pointed to by a word whose low bit says "heap-owned" (1) or
 * "borrowed from static storage" (0). Item alignment guarantees the bit is free. */
typedef struct {
    char text[24];
    int len;
} Item;

typedef uintptr_t ItemRef;

static int live_heap;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static ItemRef borrowed(const Item *it) {
    check(((uintptr_t)it & 1u) == 0, "static item alignment");
    return (uintptr_t)it;
}

static ItemRef owned_from(const char *s) {
    Item *it = calloc(1, sizeof *it);
    check(it != NULL, "alloc");
    check(((uintptr_t)it & 1u) == 0, "malloc alignment");
    check(strlen(s) < sizeof it->text, "too long");
    strcpy(it->text, s);
    it->len = (int)strlen(s);
    live_heap++;
    return (uintptr_t)it | 1u;
}

static int is_owned(ItemRef r) { return (int)(r & 1u); }
static Item *item_of(ItemRef r) { return (Item *)(r & ~(uintptr_t)1u); }

static void ref_drop(ItemRef *r) {
    if (is_owned(*r)) {
        free(item_of(*r));
        live_heap--;
    }
    *r = 0;
}

/* Ensure ownership before mutating (clone-on-write). */
static Item *ref_to_mut(ItemRef *r) {
    if (!is_owned(*r))
        *r = owned_from(item_of(*r)->text);
    return item_of(*r);
}

/* Lowercase only if needed; stays borrowed when nothing changes. */
static ItemRef normalize(ItemRef in) {
    const Item *it = item_of(in);
    int first = it->len;
    for (int i = 0; i < it->len; i++)
        if (it->text[i] >= 'A' && it->text[i] <= 'Z') {
            first = i;
            break;
        }
    if (first == it->len)
        return in; /* pass through unchanged, borrowed or owned */
    Item *w = ref_to_mut(&in);
    for (int i = first; i < w->len; i++)
        if (w->text[i] >= 'A' && w->text[i] <= 'Z')
            w->text[i] = (char)(w->text[i] + 32);
    return in;
}

static void show(const char *tag, ItemRef r) {
    printf("%-10s %-9s len=%d \"%s\"\n", tag, is_owned(r) ? "owned" : "borrowed", item_of(r)->len,
           item_of(r)->text);
}

static Item table[] = {
    {"already lower", 13}, {"MiXeD Case", 10}, {"UPPER", 5}, {"", 0}, {"tail End", 8}, {"Borrowed", 8},
};

int main(void) {
    ItemRef out[5];
    for (int i = 0; i < 5; i++) {
        ItemRef in = borrowed(&table[i]);
        out[i] = normalize(in);
        show("normalize", out[i]);
        check(is_owned(out[i]) == (strcmp(table[i].text, item_of(out[i])->text) != 0),
              "owned iff changed");
    }
    printf("heap items live: %d\n", live_heap);

    ItemRef o = owned_from("hello");
    show("owned", o);
    ItemRef o2 = normalize(o);
    check(o2 == o, "no copy for already-normal owned item");
    printf("owned passthrough live: %d\n", live_heap);

    ItemRef b = borrowed(&table[5]);
    Item *w = ref_to_mut(&b);
    w->text[0] = 'b';
    show("mutated", b);
    printf("static table unchanged: %s\n", table[5].text);

    for (int i = 0; i < 5; i++)
        ref_drop(&out[i]);
    ref_drop(&o2);
    ref_drop(&b);
    printf("heap items at end: %d\n", live_heap);
    check(live_heap == 0, "leak");
    return 0;
}
