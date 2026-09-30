/*
 * title: Pointer swizzling of an object graph to and from a flat image
 * topic: memory
 * covers: swizzle/unswizzle, index-based image, cyclic graphs, object table, image validation
 * deps: libc
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 4

typedef struct Person {
    char name[16];
    int age;
    struct Person *best;          /* may be NULL, may form cycles */
    struct Person *friends[MAXF]; /* NULL terminated by count */
    int nfriends;
} Person;

/* On-disk form: pointers replaced by 1-based indexes, 0 means NULL. */
typedef struct {
    char name[16];
    int32_t age;
    uint32_t best;
    uint32_t friends[MAXF];
    int32_t nfriends;
} PersonImg;

typedef struct {
    uint32_t magic;
    uint32_t count;
} ImgHead;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t index_of(Person *const *tab, uint32_t n, const Person *p) {
    if (!p)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        if (tab[i] == p)
            return i + 1;
    check(0, "pointer not in table");
    return 0;
}

static size_t save(Person *const *tab, uint32_t n, unsigned char *out) {
    ImgHead h = {0x50455253u, n};
    memcpy(out, &h, sizeof h);
    for (uint32_t i = 0; i < n; i++) {
        PersonImg im;
        memset(&im, 0, sizeof im);
        memcpy(im.name, tab[i]->name, sizeof im.name);
        im.age = tab[i]->age;
        im.best = index_of(tab, n, tab[i]->best);
        im.nfriends = tab[i]->nfriends;
        for (int f = 0; f < tab[i]->nfriends; f++)
            im.friends[f] = index_of(tab, n, tab[i]->friends[f]);
        memcpy(out + sizeof h + i * sizeof im, &im, sizeof im);
    }
    return sizeof h + n * sizeof(PersonImg);
}

/* Returns table of freshly allocated people or NULL if the image is invalid. */
static Person **load(const unsigned char *img, size_t len, uint32_t *n_out) {
    ImgHead h;
    if (len < sizeof h)
        return NULL;
    memcpy(&h, img, sizeof h);
    if (h.magic != 0x50455253u || len != sizeof h + (size_t)h.count * sizeof(PersonImg))
        return NULL;
    Person **tab = calloc(h.count, sizeof *tab);
    Person *pool = calloc(h.count, sizeof *pool);
    if (!tab || !pool) {
        free(tab);
        free(pool);
        return NULL;
    }
    for (uint32_t i = 0; i < h.count; i++)
        tab[i] = &pool[i];
    /* pass 1: copy plain data; pass 2: swizzle indexes into pointers */
    for (uint32_t i = 0; i < h.count; i++) {
        PersonImg im;
        memcpy(&im, img + sizeof h + i * sizeof im, sizeof im);
        if (im.best > h.count || im.nfriends < 0 || im.nfriends > MAXF)
            goto bad;
        memcpy(tab[i]->name, im.name, sizeof im.name);
        tab[i]->age = im.age;
        tab[i]->nfriends = im.nfriends;
        tab[i]->best = im.best ? tab[im.best - 1] : NULL;
        for (int f = 0; f < im.nfriends; f++) {
            if (im.friends[f] == 0 || im.friends[f] > h.count)
                goto bad;
            tab[i]->friends[f] = tab[im.friends[f] - 1];
        }
    }
    *n_out = h.count;
    return tab;
bad:
    free(pool);
    free(tab);
    return NULL;
}

static void release(Person **tab) {
    free(tab[0]);
    free(tab);
}

int main(void) {
    enum { N = 10 };
    Person *tab = calloc(N, sizeof *tab);
    Person *ptrs[N];
    check(tab != NULL, "calloc");
    static const char *names[N] = {"ada", "bob", "cy", "di", "ed", "flo", "gus", "hal", "ivy", "jo"};
    for (int i = 0; i < N; i++) {
        ptrs[i] = &tab[i];
        snprintf(tab[i].name, sizeof tab[i].name, "%s", names[i]);
        tab[i].age = 20 + i * 3;
    }
    /* a graph with cycles, self reference and dangling NULLs */
    for (int i = 0; i < N; i++) {
        tab[i].best = (i % 3 == 0) ? NULL : &tab[(i * 7 + 1) % N];
        tab[i].nfriends = (i % 4) + (i == 9 ? 0 : 1);
        for (int f = 0; f < tab[i].nfriends; f++)
            tab[i].friends[f] = &tab[(i + f * 3 + 1) % N];
    }
    tab[4].best = &tab[4]; /* self loop */

    unsigned char img[1024];
    size_t len = save(ptrs, N, img);
    printf("image bytes: %zu (%zu per person)\n", len, sizeof(PersonImg));

    uint32_t n = 0;
    Person **copy = load(img, len, &n);
    check(copy != NULL && n == N, "load");
    long agesum = 0;
    int links = 0;
    for (uint32_t i = 0; i < n; i++) {
        check(strcmp(copy[i]->name, tab[i].name) == 0, "name");
        check(copy[i]->age == tab[i].age, "age");
        check(copy[i] != &tab[i], "distinct object");
        agesum += copy[i]->age;
        if (copy[i]->best) {
            /* the swizzled pointer must stay inside the new pool */
            uint32_t k = index_of(copy, n, copy[i]->best);
            check(k > 0 && strcmp(copy[i]->best->name, tab[i].best->name) == 0, "best target");
            links++;
        }
        for (int f = 0; f < copy[i]->nfriends; f++) {
            check(strcmp(copy[i]->friends[f]->name, tab[i].friends[f]->name) == 0, "friend");
            links++;
        }
    }
    printf("people=%u age-sum=%ld links=%d\n", (unsigned)n, agesum, links);
    printf("ed.best -> %s (self loop kept: %s)\n", copy[4]->best->name, copy[4]->best == copy[4] ? "yes" : "no");

    /* save again from the loaded graph: identical bytes prove a stable round trip */
    unsigned char img2[1024];
    size_t len2 = save(copy, n, img2);
    check(len2 == len && memcmp(img, img2, len) == 0, "stable image");
    printf("second save identical: yes\n");

    /* walk the best-chain from each person with a step limit to show cycles are intact */
    for (uint32_t i = 0; i < n; i += 3) {
        const Person *p = copy[i];
        int steps = 0;
        printf("chain from %s:", p->name);
        while (p && steps < 6) {
            printf(" %s", p->name);
            p = p->best;
            steps++;
        }
        printf("%s\n", p ? " ..." : " end");
    }

    /* validation rejects damaged images */
    unsigned char bad[1024];
    memcpy(bad, img, len);
    bad[0] ^= 1;
    check(load(bad, len, &n) == NULL, "bad magic rejected");
    memcpy(bad, img, len);
    check(load(bad, len - 1, &n) == NULL, "short image rejected");
    memcpy(bad, img, len);
    uint32_t oob = 99;
    memcpy(bad + sizeof(ImgHead) + offsetof(PersonImg, best), &oob, sizeof oob);
    check(load(bad, len, &n) == NULL, "index out of range rejected");
    printf("damaged images rejected: 3\n");

    release(copy);
    free(tab);
    return 0;
}
