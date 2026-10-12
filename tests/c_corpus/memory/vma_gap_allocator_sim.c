/*
 * title: Virtual memory area list with gap search, split and merge
 * topic: memory
 * covers: address-space bookkeeping, first-fit gap allocation with alignment, partial unmap splitting, permission changes, coalescing, invariant audit
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A simulated address space (units are pages, not the host's pages), like the kernel's VMA list. */
#define SPACE_LO 16u
#define SPACE_HI 1024u
#define MAXV 256

enum { R = 1, W = 2, X = 4 };

typedef struct {
    uint32_t start, end; /* [start, end) in pages */
    uint32_t prot;
    uint32_t tag; /* which mapping request created it (kept only for stats) */
} Vma;

typedef struct {
    Vma v[MAXV];
    int n;
    unsigned splits, merges;
} Space;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void audit(const Space *s) {
    for (int i = 0; i < s->n; i++) {
        check(s->v[i].start < s->v[i].end, "non-empty");
        check(s->v[i].start >= SPACE_LO && s->v[i].end <= SPACE_HI, "inside space");
        if (i > 0) {
            check(s->v[i - 1].end <= s->v[i].start, "sorted and disjoint");
            check(!(s->v[i - 1].end == s->v[i].start && s->v[i - 1].prot == s->v[i].prot), "no mergeable neighbours");
        }
    }
}

static void insert_at(Space *s, int i, Vma x) {
    check(s->n < MAXV, "vma capacity");
    memmove(&s->v[i + 1], &s->v[i], sizeof(Vma) * (size_t)(s->n - i));
    s->v[i] = x;
    s->n++;
}

static void remove_at(Space *s, int i) {
    memmove(&s->v[i], &s->v[i + 1], sizeof(Vma) * (size_t)(s->n - i - 1));
    s->n--;
}

static void coalesce(Space *s) {
    for (int i = 0; i + 1 < s->n;) {
        if (s->v[i].end == s->v[i + 1].start && s->v[i].prot == s->v[i + 1].prot) {
            s->v[i].end = s->v[i + 1].end;
            remove_at(s, i + 1);
            s->merges++;
        } else {
            i++;
        }
    }
}

/* First-fit search for `len` pages aligned to `align` pages. Returns start or 0 when none fits. */
static uint32_t find_gap(const Space *s, uint32_t len, uint32_t align) {
    uint32_t cursor = SPACE_LO;
    for (int i = 0; i <= s->n; i++) {
        uint32_t gap_end = (i < s->n) ? s->v[i].start : SPACE_HI;
        uint32_t a = (cursor + align - 1) / align * align;
        if (a + len <= gap_end && a >= cursor)
            return a;
        if (i < s->n)
            cursor = s->v[i].end;
    }
    return 0;
}

static uint32_t do_map(Space *s, uint32_t len, uint32_t align, uint32_t prot, uint32_t tag) {
    uint32_t at = find_gap(s, len, align);
    if (!at)
        return 0;
    int i = 0;
    while (i < s->n && s->v[i].start < at)
        i++;
    Vma x = {at, at + len, prot, tag};
    insert_at(s, i, x);
    coalesce(s);
    return at;
}

/* Apply `prot` (or unmap if prot == 0xFF) to [lo, hi), splitting VMAs at the edges. */
static int change(Space *s, uint32_t lo, uint32_t hi, uint32_t prot) {
    int touched = 0;
    for (int i = 0; i < s->n; i++) {
        Vma *v = &s->v[i];
        if (v->end <= lo || v->start >= hi)
            continue;
        touched++;
        if (v->start < lo) {
            Vma right = *v;
            right.start = lo;
            v->end = lo;
            insert_at(s, i + 1, right);
            s->splits++;
            continue; /* the right half is examined on the next iteration */
        }
        if (v->end > hi) {
            Vma right = *v;
            right.start = hi;
            v->end = hi;
            insert_at(s, i + 1, right);
            s->splits++;
        }
        v = &s->v[i];
        if (prot == 0xFF) {
            remove_at(s, i);
            i--;
        } else {
            v->prot = prot;
        }
    }
    coalesce(s);
    return touched;
}

static void dump(const char *label, const Space *s) {
    printf("%s (%d areas):", label, s->n);
    for (int i = 0; i < s->n; i++)
        printf(" [%u,%u)%c%c%c", (unsigned)s->v[i].start, (unsigned)s->v[i].end, (s->v[i].prot & R) ? 'r' : '-',
               (s->v[i].prot & W) ? 'w' : '-', (s->v[i].prot & X) ? 'x' : '-');
    printf("\n");
}

static uint32_t mapped_pages(const Space *s) {
    uint32_t t = 0;
    for (int i = 0; i < s->n; i++)
        t += s->v[i].end - s->v[i].start;
    return t;
}

static uint32_t rs = 555;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

int main(void) {
    Space sp;
    memset(&sp, 0, sizeof sp);

    /* scripted scenario */
    uint32_t a = do_map(&sp, 8, 1, R | X, 1);
    uint32_t b = do_map(&sp, 16, 1, R | W, 2);
    uint32_t c = do_map(&sp, 4, 8, R | W, 3);
    printf("mapped at %u, %u, %u (aligned to 8: %s)\n", (unsigned)a, (unsigned)b, (unsigned)c, c % 8 == 0 ? "yes" : "no");
    dump("initial", &sp);
    check(a == SPACE_LO && b == a + 8, "first-fit placement");
    audit(&sp);

    /* adjacent mappings with equal protection coalesce into one area */
    uint32_t d = do_map(&sp, 6, 1, R | W, 4);
    dump("after adding rw neighbour", &sp);
    check(d != 0, "mapped d");
    audit(&sp);

    /* punch a hole in the middle of the rw area: one area becomes two */
    unsigned before = sp.splits;
    change(&sp, b + 4, b + 9, 0xFF);
    printf("punched hole [%u,%u): splits+%u\n", (unsigned)(b + 4), (unsigned)(b + 9), sp.splits - before);
    dump("after hole", &sp);
    audit(&sp);

    /* mprotect a range spanning several areas, then a sub-range back */
    change(&sp, a + 2, b + 2, R);
    dump("after mprotect r over [a+2,b+2)", &sp);
    change(&sp, a + 2, a + 4, R | X);
    dump("after restoring rx on 2 pages", &sp);
    audit(&sp);

    /* an allocation that only fits in the punched hole */
    uint32_t hole = do_map(&sp, 5, 1, R, 5);
    printf("hole filled at %u\n", (unsigned)hole);
    dump("after hole fill", &sp);
    audit(&sp);

    /* stress with random maps, unmaps and protection changes; audit each round */
    unsigned maps = 0, failed = 0, unmaps = 0, prots = 0;
    for (int step = 0; step < 3000; step++) {
        uint32_t r = rnd();
        uint32_t op = r % 6;
        if (op < 3) {
            uint32_t len = 1 + (r >> 8) % 24;
            uint32_t al = 1u << ((r >> 16) % 4);
            uint32_t prot = (uint32_t[]){R, R | W, R | X, R | W | X}[(r >> 20) % 4];
            uint32_t at = do_map(&sp, len, al, prot, (uint32_t)step);
            if (at) {
                check(at % al == 0, "alignment honoured");
                maps++;
            } else {
                failed++;
            }
        } else if (op < 5) {
            uint32_t lo = SPACE_LO + (r >> 8) % (SPACE_HI - SPACE_LO - 40);
            uint32_t len = 1 + (r >> 20) % 30;
            change(&sp, lo, lo + len, 0xFF);
            unmaps++;
        } else {
            uint32_t lo = SPACE_LO + (r >> 8) % (SPACE_HI - SPACE_LO - 40);
            uint32_t len = 1 + (r >> 20) % 30;
            change(&sp, lo, lo + len, (uint32_t[]){R, R | W, R | X}[(r >> 4) % 3]);
            prots++;
        }
        audit(&sp);
    }
    printf("stress: maps=%u failed=%u unmaps=%u protects=%u\n", maps, failed, unmaps, prots);
    printf("final: %d areas, %u pages mapped, splits=%u merges=%u\n", sp.n, (unsigned)mapped_pages(&sp), sp.splits,
           sp.merges);

    /* largest free gap */
    uint32_t cursor = SPACE_LO, best = 0;
    for (int i = 0; i <= sp.n; i++) {
        uint32_t e = (i < sp.n) ? sp.v[i].start : SPACE_HI;
        if (e - cursor > best)
            best = e - cursor;
        if (i < sp.n)
            cursor = sp.v[i].end;
    }
    printf("largest free gap: %u pages\n", (unsigned)best);
    check(mapped_pages(&sp) + 0 <= SPACE_HI - SPACE_LO, "mapped within space");
    /* the largest gap really is allocatable, and one page more is not */
    uint32_t fit = find_gap(&sp, best, 1);
    check(fit != 0, "largest gap fits");
    check(find_gap(&sp, best + 1, 1) == 0, "one more does not fit");
    printf("largest gap allocatable: yes, one page larger: no\n");
    return 0;
}
