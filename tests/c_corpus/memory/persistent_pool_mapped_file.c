/*
 * title: Persistent object pool with index handles in a mapped file
 * topic: memory
 * covers: fixed-size slot pool, persistent free list, generation-tagged handles, intrusive list by index, reopen and audit
 * deps: posix
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define NIL 0xFFFFFFFFu
#define MAGIC 0x504F4F4Cu

typedef struct {
    uint32_t next;       /* free list link when free, list link when live */
    uint32_t prev;       /* only meaningful when live */
    uint32_t generation; /* bumped on every free */
    uint32_t live;
    char name[16];
    int64_t balance;
} Slot; /* 40 bytes */

typedef struct {
    uint32_t magic;
    uint32_t nslots;
    uint32_t free_head;
    uint32_t live_head;
    uint32_t live_count;
    uint32_t pad[3];
} Head; /* 32 bytes */

typedef uint64_t Handle; /* generation << 32 | index */

typedef struct {
    int fd;
    size_t bytes;
    Head *h;
    Slot *s;
} Pool;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void pool_map(Pool *p, int fd, uint32_t nslots) {
    p->fd = fd;
    p->bytes = sizeof(Head) + (size_t)nslots * sizeof(Slot);
    void *m = mmap(NULL, p->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(m != MAP_FAILED, "mmap");
    p->h = m;
    p->s = (Slot *)((char *)m + sizeof(Head));
}

static void pool_create(Pool *p, const char *path, uint32_t nslots) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open");
    check(ftruncate(fd, (off_t)(sizeof(Head) + (size_t)nslots * sizeof(Slot))) == 0, "ftruncate");
    pool_map(p, fd, nslots);
    p->h->magic = MAGIC;
    p->h->nslots = nslots;
    p->h->live_head = NIL;
    p->h->live_count = 0;
    p->h->free_head = 0;
    for (uint32_t i = 0; i < nslots; i++) {
        p->s[i].next = i + 1 < nslots ? i + 1 : NIL;
        p->s[i].generation = 1;
        p->s[i].live = 0;
    }
}

static void pool_reopen(Pool *p, const char *path) {
    int fd = open(path, O_RDWR);
    check(fd >= 0, "reopen");
    Head h;
    check(pread(fd, &h, sizeof h, 0) == (ssize_t)sizeof h && h.magic == MAGIC, "head");
    pool_map(p, fd, h.nslots);
}

static void pool_close(Pool *p) {
    check(msync(p->h, p->bytes, MS_SYNC) == 0, "msync");
    check(munmap(p->h, p->bytes) == 0, "munmap");
    close(p->fd);
}

static Handle pool_alloc(Pool *p, const char *name, int64_t balance) {
    uint32_t i = p->h->free_head;
    if (i == NIL)
        return 0;
    Slot *s = &p->s[i];
    p->h->free_head = s->next;
    s->live = 1;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->balance = balance;
    s->prev = NIL;
    s->next = p->h->live_head;
    if (s->next != NIL)
        p->s[s->next].prev = i;
    p->h->live_head = i;
    p->h->live_count++;
    return ((Handle)s->generation << 32) | i;
}

static Slot *pool_get(Pool *p, Handle h) {
    uint32_t i = (uint32_t)h, g = (uint32_t)(h >> 32);
    if (i >= p->h->nslots || !p->s[i].live || p->s[i].generation != g)
        return NULL;
    return &p->s[i];
}

static int pool_free(Pool *p, Handle h) {
    Slot *s = pool_get(p, h);
    if (!s)
        return 0;
    uint32_t i = (uint32_t)h;
    if (s->prev != NIL)
        p->s[s->prev].next = s->next;
    else
        p->h->live_head = s->next;
    if (s->next != NIL)
        p->s[s->next].prev = s->prev;
    s->live = 0;
    s->generation++;
    s->next = p->h->free_head;
    p->h->free_head = i;
    p->h->live_count--;
    return 1;
}

/* structural audit: free and live lists partition the slots, links are consistent */
static void audit(const Pool *p, uint32_t *live_out, int64_t *sum_out) {
    uint32_t n = p->h->nslots;
    unsigned char *seen = calloc(n, 1);
    check(seen != NULL, "calloc");
    uint32_t live = 0, freec = 0;
    int64_t sum = 0;
    uint32_t prev = NIL;
    for (uint32_t i = p->h->live_head; i != NIL; i = p->s[i].next) {
        check(i < n && !seen[i], "live list valid");
        seen[i] = 1;
        check(p->s[i].live == 1 && p->s[i].prev == prev, "back link");
        prev = i;
        live++;
        sum += p->s[i].balance;
    }
    for (uint32_t i = p->h->free_head; i != NIL; i = p->s[i].next) {
        check(i < n && !seen[i], "free list valid");
        seen[i] = 1;
        check(p->s[i].live == 0, "free slot not live");
        freec++;
    }
    check(live + freec == n && live == p->h->live_count, "partition");
    free(seen);
    *live_out = live;
    *sum_out = sum;
}

static uint64_t rs = 909;

static uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void) {
    char path[] = "mmpool_XXXXXX";
    int t = mkstemp(path);
    check(t >= 0, "mkstemp");
    close(t);

    enum { N = 64, HOLD = 40 };
    Pool p;
    pool_create(&p, path, N);
    Handle held[HOLD];
    memset(held, 0, sizeof held);
    int64_t model_sum = 0;
    int64_t model_bal[HOLD];
    unsigned long allocs = 0, frees = 0, stale_rejected = 0, full = 0;
    Handle graveyard[16];
    unsigned ng = 0;
    for (int step = 0; step < 3000; step++) {
        uint64_t r = rnd();
        int slot = (int)(r % HOLD);
        if (held[slot]) {
            Handle old = held[slot];
            check(pool_free(&p, old), "free");
            model_sum -= model_bal[slot];
            held[slot] = 0;
            frees++;
            graveyard[ng++ % 16] = old;
            /* a stale handle must never resolve, even if the slot is reused */
            check(pool_get(&p, old) == NULL, "stale rejected");
            stale_rejected++;
        } else {
            char name[16];
            int64_t bal = (int64_t)(r >> 32) % 1000 - 500;
            snprintf(name, sizeof name, "acct%d", slot);
            Handle h = pool_alloc(&p, name, bal);
            if (!h) {
                full++;
                continue;
            }
            held[slot] = h;
            model_bal[slot] = bal;
            model_sum += bal;
            allocs++;
        }
        if (step % 700 == 0) {
            uint32_t live;
            int64_t sum;
            audit(&p, &live, &sum);
            check(sum == model_sum, "sum mid-run");
        }
    }
    for (unsigned i = 0; i < 16 && i < ng; i++)
        check(pool_get(&p, graveyard[i]) == NULL, "old handles remain dead");
    uint32_t live;
    int64_t sum;
    audit(&p, &live, &sum);
    printf("allocs=%lu frees=%lu stale-rejected=%lu full=%lu\n", allocs, frees, stale_rejected, full);
    printf("live=%u balance-sum=%lld\n", (unsigned)live, (long long)sum);
    check(sum == model_sum, "sum");

    /* close, reopen, and confirm every held handle still resolves to its data */
    pool_close(&p);
    pool_reopen(&p, path);
    uint32_t live2;
    int64_t sum2;
    audit(&p, &live2, &sum2);
    check(live2 == live && sum2 == sum, "audit after reopen");
    unsigned resolved = 0;
    for (int i = 0; i < HOLD; i++) {
        if (!held[i])
            continue;
        Slot *s = pool_get(&p, held[i]);
        check(s != NULL && s->balance == model_bal[i], "handle resolves after reopen");
        char want[16];
        snprintf(want, sizeof want, "acct%d", i);
        check(strcmp(s->name, want) == 0, "name");
        resolved++;
    }
    printf("after reopen: %u handles resolved, audit live=%u\n", resolved, (unsigned)live2);

    /* generations: max generation seen */
    uint32_t maxgen = 0;
    for (uint32_t i = 0; i < N; i++)
        if (p.s[i].generation > maxgen)
            maxgen = p.s[i].generation;
    printf("max slot generation: %u\n", (unsigned)maxgen);

    /* drain everything */
    for (int i = 0; i < HOLD; i++)
        if (held[i])
            check(pool_free(&p, held[i]), "drain");
    audit(&p, &live2, &sum2);
    check(live2 == 0 && sum2 == 0, "empty");
    printf("drained: live=%u\n", (unsigned)live2);
    pool_close(&p);
    unlink(path);
    return 0;
}
